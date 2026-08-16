use std::collections::{HashMap, HashSet};
use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver};
use std::thread::JoinHandle;
use std::time::Duration;

use eframe::egui;
use fast_sigma_raw::{ConversionOptions, ConversionReport, convert, default_output_path, is_x3f};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

const MEDIA_POLL_INTERVAL: Duration = Duration::from_secs(2);

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
enum Destination {
    #[default]
    BesideOriginals,
    ChosenDirectory,
}

enum WorkerMessage {
    Started {
        index: usize,
        total: usize,
        input: PathBuf,
    },
    Finished {
        input: PathBuf,
        result: Result<ConversionReport, String>,
    },
    AllDone,
}

struct CompletedJob {
    input: PathBuf,
    result: Result<ConversionReport, String>,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(default)]
struct AppSettings {
    automatic_import: bool,
    import_directory: Option<PathBuf>,
    eject_after_import: bool,
}

impl Default for AppSettings {
    fn default() -> Self {
        Self {
            automatic_import: false,
            import_directory: None,
            eject_after_import: true,
        }
    }
}

#[derive(Debug, Deserialize, Serialize)]
struct ImportRecord {
    sha256: String,
    output: PathBuf,
}

enum AutoImportMessage {
    Scanning(PathBuf),
    Started(PathBuf),
    Imported {
        input: PathBuf,
        report: ConversionReport,
    },
    Skipped(PathBuf),
    Failed {
        input: PathBuf,
        error: String,
    },
    CardFinished {
        volume: PathBuf,
        imported: usize,
        skipped: usize,
        had_failures: bool,
        eject_result: Option<Result<(), String>>,
    },
    Stopped,
}

struct AutoImporter {
    stop: Arc<AtomicBool>,
    receiver: Receiver<AutoImportMessage>,
    _thread: JoinHandle<()>,
}

struct ConverterApp {
    inputs: Vec<PathBuf>,
    destination: Destination,
    output_directory: Option<PathBuf>,
    overwrite: bool,
    receiver: Option<Receiver<WorkerMessage>>,
    current_file: Option<PathBuf>,
    completed_count: usize,
    total_count: usize,
    completed: Vec<CompletedJob>,
    notice: Option<String>,
    automatic_import: bool,
    import_directory: Option<PathBuf>,
    eject_after_import: bool,
    auto_importer: Option<AutoImporter>,
    auto_status: String,
    auto_imported_count: usize,
    auto_skipped_count: usize,
}

impl ConverterApp {
    fn new() -> Self {
        let settings = load_settings();
        let mut app = Self {
            inputs: Vec::new(),
            destination: Destination::BesideOriginals,
            output_directory: None,
            overwrite: false,
            receiver: None,
            current_file: None,
            completed_count: 0,
            total_count: 0,
            completed: Vec::new(),
            notice: None,
            automatic_import: settings.automatic_import,
            import_directory: settings.import_directory,
            eject_after_import: settings.eject_after_import,
            auto_importer: None,
            auto_status: "Automatic import is off.".into(),
            auto_imported_count: 0,
            auto_skipped_count: 0,
        };
        if app.automatic_import
            && let Err(error) = app.start_auto_importer()
        {
            app.automatic_import = false;
            app.notice = Some(error);
        }
        app
    }

    fn add_paths(&mut self, paths: impl IntoIterator<Item = PathBuf>) {
        let mut ignored = 0;
        for path in paths {
            if !is_x3f(&path) {
                ignored += 1;
                continue;
            }
            if !self.inputs.contains(&path) {
                self.inputs.push(path);
            }
        }
        self.notice = (ignored > 0).then(|| {
            format!(
                "Ignored {ignored} item{} that was not an X3F file.",
                if ignored == 1 { "" } else { "s" }
            )
        });
    }

    fn output_directory(&self) -> Option<&Path> {
        match self.destination {
            Destination::BesideOriginals => None,
            Destination::ChosenDirectory => self.output_directory.as_deref(),
        }
    }

    fn validate_batch(&self) -> Result<(), String> {
        if self.inputs.is_empty() {
            return Err("Add at least one X3F file first.".into());
        }
        if self.destination == Destination::ChosenDirectory && self.output_directory.is_none() {
            return Err("Choose an output directory first.".into());
        }
        let outputs: Vec<_> = self
            .inputs
            .iter()
            .map(|input| default_output_path(input, self.output_directory()))
            .collect();
        let unique: HashSet<_> = outputs.iter().collect();
        if unique.len() != outputs.len() {
            return Err(
                "Two selected files have the same name. Export them beside their originals or convert them separately."
                    .into(),
            );
        }
        if !self.overwrite
            && let Some(existing) = outputs.iter().find(|output| output.exists())
        {
            return Err(format!(
                "{} already exists. Choose another folder or enable replacement.",
                existing.display()
            ));
        }
        Ok(())
    }

    fn start(&mut self) {
        if let Err(error) = self.validate_batch() {
            self.notice = Some(error);
            return;
        }
        let inputs = self.inputs.clone();
        let output_directory = self.output_directory().map(Path::to_path_buf);
        let overwrite = self.overwrite;
        let total = inputs.len();
        let (sender, receiver) = mpsc::channel();
        std::thread::spawn(move || {
            for (index, input) in inputs.into_iter().enumerate() {
                if sender
                    .send(WorkerMessage::Started {
                        index,
                        total,
                        input: input.clone(),
                    })
                    .is_err()
                {
                    return;
                }
                let output = default_output_path(&input, output_directory.as_deref());
                let options = ConversionOptions {
                    overwrite,
                    ..ConversionOptions::default()
                };
                let result = convert(&input, &output, options);
                if sender
                    .send(WorkerMessage::Finished { input, result })
                    .is_err()
                {
                    return;
                }
            }
            let _ = sender.send(WorkerMessage::AllDone);
        });
        self.receiver = Some(receiver);
        self.current_file = None;
        self.completed_count = 0;
        self.total_count = total;
        self.completed.clear();
        self.notice = None;
    }

    fn receive_updates(&mut self) {
        let messages: Vec<_> = self
            .receiver
            .as_ref()
            .map(|receiver| receiver.try_iter().collect())
            .unwrap_or_default();
        for message in messages {
            match message {
                WorkerMessage::Started {
                    index,
                    total,
                    input,
                } => {
                    self.completed_count = index;
                    self.total_count = total;
                    self.current_file = Some(input);
                }
                WorkerMessage::Finished { input, result } => {
                    self.completed_count += 1;
                    self.completed.push(CompletedJob { input, result });
                }
                WorkerMessage::AllDone => {
                    self.current_file = None;
                    self.receiver = None;
                }
            }
        }
    }

    fn choose_files(&mut self) {
        if let Some(paths) = rfd::FileDialog::new()
            .add_filter("SIGMA X3F", &["x3f", "X3F"])
            .pick_files()
        {
            self.add_paths(paths);
        }
    }

    fn choose_output_directory(&mut self) {
        let dialog = self
            .output_directory
            .as_ref()
            .map_or_else(rfd::FileDialog::new, |directory| {
                rfd::FileDialog::new().set_directory(directory)
            });
        if let Some(directory) = dialog.pick_folder() {
            self.output_directory = Some(directory);
            self.destination = Destination::ChosenDirectory;
        }
    }

    fn choose_import_directory(&mut self) {
        let dialog = self
            .import_directory
            .as_ref()
            .map_or_else(rfd::FileDialog::new, |directory| {
                rfd::FileDialog::new().set_directory(directory)
            });
        if let Some(directory) = dialog.pick_folder() {
            self.import_directory = Some(directory);
            self.save_settings();
        }
    }

    fn save_settings(&mut self) {
        let settings = AppSettings {
            automatic_import: self.automatic_import,
            import_directory: self.import_directory.clone(),
            eject_after_import: self.eject_after_import,
        };
        if let Err(error) = write_settings(&settings) {
            self.notice = Some(error);
        }
    }

    fn start_auto_importer(&mut self) -> Result<(), String> {
        if self.auto_importer.is_some() {
            return Ok(());
        }
        let destination = self
            .import_directory
            .clone()
            .ok_or("Choose an automatic import folder first.")?;
        if !destination.is_dir() {
            return Err(format!(
                "Automatic import folder does not exist: {}",
                destination.display()
            ));
        }
        let stop = Arc::new(AtomicBool::new(false));
        let (sender, receiver) = mpsc::channel();
        let thread_stop = Arc::clone(&stop);
        let eject_after_import = self.eject_after_import;
        let thread = std::thread::spawn(move || {
            automatic_import_loop(destination, eject_after_import, thread_stop, sender);
        });
        self.auto_importer = Some(AutoImporter {
            stop,
            receiver,
            _thread: thread,
        });
        self.auto_status = "Watching for removable media…".into();
        Ok(())
    }

    fn set_automatic_import(&mut self, enabled: bool) {
        if enabled {
            match self.start_auto_importer() {
                Ok(()) => {
                    self.automatic_import = true;
                    self.notice = None;
                }
                Err(error) => {
                    self.automatic_import = false;
                    self.notice = Some(error);
                }
            }
        } else {
            self.automatic_import = false;
            if let Some(importer) = &self.auto_importer {
                importer.stop.store(true, Ordering::Relaxed);
            }
            self.auto_status = "Stopping automatic import…".into();
        }
        self.save_settings();
    }

    fn receive_auto_updates(&mut self) {
        let messages: Vec<_> = self
            .auto_importer
            .as_ref()
            .map(|importer| importer.receiver.try_iter().collect())
            .unwrap_or_default();
        for message in messages {
            match message {
                AutoImportMessage::Scanning(volume) => {
                    self.auto_status = format!("Scanning {}…", volume.display());
                }
                AutoImportMessage::Started(input) => {
                    self.auto_status = format!("Converting {}…", file_label(&input));
                }
                AutoImportMessage::Imported { input, report } => {
                    self.auto_imported_count += 1;
                    self.auto_status = format!(
                        "Imported {} to {}",
                        file_label(&input),
                        report.output.display()
                    );
                }
                AutoImportMessage::Skipped(input) => {
                    self.auto_skipped_count += 1;
                    self.auto_status = format!("Already imported: {}", file_label(&input));
                }
                AutoImportMessage::Failed { input, error } => {
                    self.auto_status = format!("Could not import {}", file_label(&input));
                    self.notice = Some(format!("{}: {error}", input.display()));
                }
                AutoImportMessage::CardFinished {
                    volume,
                    imported,
                    skipped,
                    had_failures,
                    eject_result,
                } => {
                    let card = file_label(&volume);
                    self.auto_status = if had_failures {
                        format!("{card} needs attention and was not ejected")
                    } else {
                        match eject_result {
                            Some(Ok(())) => format!(
                                "{card} finished and was ejected: {imported} imported, {skipped} already present"
                            ),
                            Some(Err(error)) => {
                                self.notice = Some(error);
                                format!("{card} finished but could not be ejected")
                            }
                            None => format!(
                                "{card} finished: {imported} imported, {skipped} already present"
                            ),
                        }
                    };
                }
                AutoImportMessage::Stopped => {
                    self.auto_importer = None;
                    self.auto_status = "Automatic import is off.".into();
                }
            }
        }
    }
}

impl Drop for ConverterApp {
    fn drop(&mut self) {
        if let Some(importer) = &self.auto_importer {
            importer.stop.store(true, Ordering::Relaxed);
        }
    }
}

fn file_label(path: &Path) -> String {
    path.file_name()
        .unwrap_or(path.as_os_str())
        .to_string_lossy()
        .into_owned()
}

fn config_directory() -> Option<PathBuf> {
    #[cfg(target_os = "macos")]
    {
        std::env::var_os("HOME")
            .map(|home| PathBuf::from(home).join("Library/Application Support/Fast Sigma Raw"))
    }
    #[cfg(not(target_os = "macos"))]
    {
        std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
            .map(|directory| directory.join("fast-sigma-raw"))
    }
}

fn load_settings() -> AppSettings {
    config_directory()
        .and_then(|directory| fs::read(directory.join("settings.json")).ok())
        .and_then(|bytes| serde_json::from_slice(&bytes).ok())
        .unwrap_or_default()
}

fn write_settings(settings: &AppSettings) -> Result<(), String> {
    let directory = config_directory().ok_or("Cannot find the user configuration directory.")?;
    fs::create_dir_all(&directory)
        .map_err(|error| format!("Cannot create {}: {error}", directory.display()))?;
    let output = directory.join("settings.json");
    let temporary = directory.join("settings.json.tmp");
    let json = serde_json::to_vec_pretty(settings)
        .map_err(|error| format!("Cannot encode automatic import settings: {error}"))?;
    fs::write(&temporary, json)
        .map_err(|error| format!("Cannot write {}: {error}", temporary.display()))?;
    fs::rename(&temporary, &output)
        .map_err(|error| format!("Cannot save {}: {error}", output.display()))
}

fn history_path() -> Option<PathBuf> {
    config_directory().map(|directory| directory.join("import-history.jsonl"))
}

fn load_import_history() -> HashMap<String, PathBuf> {
    let Some(path) = history_path() else {
        return HashMap::new();
    };
    let Ok(file) = File::open(path) else {
        return HashMap::new();
    };
    BufReader::new(file)
        .lines()
        .map_while(Result::ok)
        .filter_map(|line| serde_json::from_str::<ImportRecord>(&line).ok())
        .map(|record| (record.sha256, record.output))
        .collect()
}

fn append_import_history(record: &ImportRecord) -> Result<(), String> {
    let path = history_path().ok_or("Cannot find the user configuration directory.")?;
    if let Some(directory) = path.parent() {
        fs::create_dir_all(directory)
            .map_err(|error| format!("Cannot create {}: {error}", directory.display()))?;
    }
    let mut file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .map_err(|error| format!("Cannot open {}: {error}", path.display()))?;
    serde_json::to_writer(&mut file, record)
        .map_err(|error| format!("Cannot update import history: {error}"))?;
    file.write_all(b"\n")
        .map_err(|error| format!("Cannot update import history: {error}"))
}

fn mounted_media() -> HashSet<PathBuf> {
    #[cfg(target_os = "macos")]
    let roots = vec![PathBuf::from("/Volumes")];
    #[cfg(not(target_os = "macos"))]
    let roots = {
        let user = std::env::var_os("USER").unwrap_or_default();
        vec![
            PathBuf::from("/media").join(&user),
            PathBuf::from("/run/media").join(user),
        ]
    };

    roots
        .into_iter()
        .filter_map(|root| fs::read_dir(root).ok())
        .flat_map(|entries| entries.filter_map(Result::ok))
        .map(|entry| entry.path())
        .filter(|path| path.is_dir())
        .collect()
}

fn is_sigma_dcim_directory(name: &str) -> bool {
    let name = name.as_bytes();
    name.len() == 8
        && name[..3].iter().all(u8::is_ascii_digit)
        && name[3..].eq_ignore_ascii_case(b"SIGMA")
}

fn sigma_card_x3f_files(volume: &Path) -> Vec<PathBuf> {
    // A Merrill-formatted card stores images directly under
    // DCIM/NNNSIGMA. Inspecting only DCIM and these exact child directories
    // avoids recursively walking unrelated removable disks and network mounts.
    let dcim = volume.join("DCIM");
    let Ok(directories) = fs::read_dir(dcim) else {
        return Vec::new();
    };
    let mut found = Vec::new();
    for directory in directories.filter_map(Result::ok) {
        let Ok(kind) = directory.file_type() else {
            continue;
        };
        let name = directory.file_name();
        if !kind.is_dir() || !is_sigma_dcim_directory(&name.to_string_lossy()) {
            continue;
        }
        let Ok(entries) = fs::read_dir(directory.path()) else {
            continue;
        };
        found.extend(
            entries
                .filter_map(Result::ok)
                .map(|entry| entry.path())
                .filter(|path| is_x3f(path)),
        );
    }
    found.sort();
    found
}

#[cfg(target_os = "macos")]
fn eject_media(volume: &Path) -> Result<(), String> {
    let output = Command::new("diskutil")
        .arg("eject")
        .arg(volume)
        .output()
        .map_err(|error| {
            format!(
                "Could not run diskutil to eject {}: {error}",
                volume.display()
            )
        })?;
    if output.status.success() {
        Ok(())
    } else {
        let detail = String::from_utf8_lossy(&output.stderr).trim().to_owned();
        Err(format!(
            "Could not eject {}{}{}",
            volume.display(),
            if detail.is_empty() { "" } else { ": " },
            detail
        ))
    }
}

#[cfg(not(target_os = "macos"))]
fn eject_media(volume: &Path) -> Result<(), String> {
    let source = Command::new("findmnt")
        .args(["-n", "-o", "SOURCE", "--target"])
        .arg(volume)
        .output()
        .map_err(|error| format!("Could not identify {}: {error}", volume.display()))?;
    if !source.status.success() {
        return Err(format!(
            "Could not identify the device for {}",
            volume.display()
        ));
    }
    let source = String::from_utf8_lossy(&source.stdout).trim().to_owned();
    let output = Command::new("udisksctl")
        .args(["unmount", "-b", &source])
        .output()
        .map_err(|error| format!("Could not run udisksctl for {}: {error}", volume.display()))?;
    if output.status.success() {
        Ok(())
    } else {
        let detail = String::from_utf8_lossy(&output.stderr).trim().to_owned();
        Err(format!("Could not unmount {}: {detail}", volume.display()))
    }
}

#[cfg(target_os = "macos")]
fn send_notification(message: &str) {
    let _ = Command::new("osascript")
        .args([
            "-e",
            "on run argv",
            "-e",
            "display notification (item 1 of argv) with title \"Fast Sigma Raw\"",
            "-e",
            "end run",
            "--",
            message,
        ])
        .status();
}

#[cfg(not(target_os = "macos"))]
fn send_notification(message: &str) {
    let _ = Command::new("notify-send")
        .args(["Fast Sigma Raw", message])
        .status();
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let mut file = File::open(path).map_err(|error| {
        format!(
            "Cannot read {} for duplicate detection: {error}",
            path.display()
        )
    })?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0_u8; 1024 * 1024];
    loop {
        let count = file
            .read(&mut buffer)
            .map_err(|error| format!("Cannot read {}: {error}", path.display()))?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(format!("{:x}", hasher.finalize()))
}

enum ImportDestination {
    Convert(PathBuf),
    Existing(PathBuf),
}

fn import_destination(
    input: &Path,
    destination: &Path,
    known_outputs: &HashSet<PathBuf>,
) -> ImportDestination {
    let base = default_output_path(input, Some(destination));
    if !base.exists() {
        return ImportDestination::Convert(base);
    }
    if !known_outputs.contains(&base) {
        return ImportDestination::Existing(base);
    }

    let stem = input
        .file_stem()
        .map(|value| value.to_string_lossy())
        .unwrap_or_default();
    for suffix in 2.. {
        let candidate = destination.join(format!("{stem}-{suffix}.dng"));
        if !candidate.exists() {
            return ImportDestination::Convert(candidate);
        }
    }
    unreachable!()
}

fn automatic_import_loop(
    destination: PathBuf,
    eject_after_import: bool,
    stop: Arc<AtomicBool>,
    sender: mpsc::Sender<AutoImportMessage>,
) {
    let mut history = load_import_history();
    let mut known_volumes = HashSet::new();
    while !stop.load(Ordering::Relaxed) {
        let volumes = mounted_media();
        for volume in volumes.difference(&known_volumes) {
            let inputs = sigma_card_x3f_files(volume);
            if inputs.is_empty() {
                continue;
            }
            let mut imported_count = 0;
            let mut skipped_count = 0;
            let mut failed = false;
            if sender
                .send(AutoImportMessage::Scanning(volume.clone()))
                .is_err()
            {
                return;
            }
            for input in inputs {
                if stop.load(Ordering::Relaxed) {
                    break;
                }
                let digest = match sha256_file(&input) {
                    Ok(digest) => digest,
                    Err(error) => {
                        failed = true;
                        let _ = sender.send(AutoImportMessage::Failed { input, error });
                        continue;
                    }
                };
                if history.contains_key(&digest) {
                    skipped_count += 1;
                    let _ = sender.send(AutoImportMessage::Skipped(input));
                    continue;
                }

                let known_outputs: HashSet<_> = history.values().cloned().collect();
                let output = match import_destination(&input, &destination, &known_outputs) {
                    ImportDestination::Convert(output) => output,
                    ImportDestination::Existing(output) => {
                        let record = ImportRecord {
                            sha256: digest.clone(),
                            output: output.clone(),
                        };
                        if let Err(error) = append_import_history(&record) {
                            failed = true;
                            let _ = sender.send(AutoImportMessage::Failed { input, error });
                            continue;
                        }
                        history.insert(digest, output);
                        skipped_count += 1;
                        let _ = sender.send(AutoImportMessage::Skipped(input));
                        continue;
                    }
                };
                if sender
                    .send(AutoImportMessage::Started(input.clone()))
                    .is_err()
                {
                    return;
                }
                match convert(&input, &output, ConversionOptions::default()) {
                    Ok(report) => {
                        let record = ImportRecord {
                            sha256: digest.clone(),
                            output: report.output.clone(),
                        };
                        if let Err(error) = append_import_history(&record) {
                            failed = true;
                            let _ = sender.send(AutoImportMessage::Failed { input, error });
                            continue;
                        }
                        history.insert(digest, report.output.clone());
                        imported_count += 1;
                        let _ = sender.send(AutoImportMessage::Imported { input, report });
                    }
                    Err(error) => {
                        failed = true;
                        let _ = sender.send(AutoImportMessage::Failed { input, error });
                    }
                }
            }
            if stop.load(Ordering::Relaxed) {
                continue;
            }
            let card = file_label(volume);
            let eject_result = if failed {
                send_notification(&format!(
                    "Import from {card} needs attention. The card was not ejected."
                ));
                None
            } else if eject_after_import {
                let result = eject_media(volume);
                match &result {
                    Ok(()) => send_notification(&format!(
                        "Import from {card} is complete. The card was ejected safely."
                    )),
                    Err(_) => send_notification(&format!(
                        "Import from {card} is complete, but the card could not be ejected."
                    )),
                }
                Some(result)
            } else {
                send_notification(&format!("Import from {card} is complete."));
                None
            };
            let _ = sender.send(AutoImportMessage::CardFinished {
                volume: volume.clone(),
                imported: imported_count,
                skipped: skipped_count,
                had_failures: failed,
                eject_result,
            });
        }
        known_volumes = volumes;
        let slices = MEDIA_POLL_INTERVAL.as_millis() / 100;
        for _ in 0..slices {
            if stop.load(Ordering::Relaxed) {
                break;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
    }
    let _ = sender.send(AutoImportMessage::Stopped);
}

impl eframe::App for ConverterApp {
    fn ui(&mut self, root_ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let context = root_ui.ctx().clone();
        self.receive_updates();
        self.receive_auto_updates();
        let busy = self.receiver.is_some();
        let dropped: Vec<PathBuf> = context.input(|input| {
            input
                .raw
                .dropped_files
                .iter()
                .map(|file| file.path().to_path_buf())
                .collect()
        });
        if !busy && !dropped.is_empty() {
            self.add_paths(dropped);
        }
        let hovering = context.input(|input| !input.raw.hovered_files.is_empty());

        egui::CentralPanel::default().show(root_ui, |ui| {
            ui.heading("Fast Sigma Raw");
            ui.label("Convert Merrill X3F files to metadata-rich linear DNG files.");
            ui.add_space(8.0);

            egui::Frame::new()
                .fill(ui.visuals().faint_bg_color)
                .stroke(ui.visuals().widgets.noninteractive.bg_stroke)
                .corner_radius(8)
                .inner_margin(14)
                .show(ui, |ui| {
                    ui.set_min_width(ui.available_width());
                    ui.strong("Automatic media import");
                    ui.label("Watch SIGMA camera cards for X3F files.");
                    ui.horizontal(|ui| {
                        ui.label(
                            self.import_directory
                                .as_ref()
                                .map(|path| path.display().to_string())
                                .unwrap_or_else(|| "No destination selected".into()),
                        );
                        if ui
                            .add_enabled(
                                !self.automatic_import,
                                egui::Button::new("Choose destination…"),
                            )
                            .clicked()
                        {
                            self.choose_import_directory();
                        }
                    });
                    let mut enabled = self.automatic_import;
                    if ui
                        .add_enabled(
                            self.auto_importer.is_none() || self.automatic_import,
                            egui::Checkbox::new(
                                &mut enabled,
                                "Automatically import from removable media",
                            ),
                        )
                        .changed()
                    {
                        self.set_automatic_import(enabled);
                    }
                    ui.label(&self.auto_status);
                    if self.auto_imported_count > 0 || self.auto_skipped_count > 0 {
                        ui.small(format!(
                            "This session: {} imported, {} already present",
                            self.auto_imported_count, self.auto_skipped_count
                        ));
                    }
                    if ui
                        .add_enabled(
                            !self.automatic_import,
                            egui::Checkbox::new(
                                &mut self.eject_after_import,
                                "Eject the card and notify me when finished",
                            ),
                        )
                        .changed()
                    {
                        self.save_settings();
                    }
                    if self.automatic_import && ui.button("Minimize and keep watching").clicked() {
                        context.send_viewport_cmd(egui::ViewportCommand::Minimized(true));
                    }
                    ui.small("Only DCIM/NNNSIGMA card folders are checked. Originals stay on the media, and duplicate content is never imported twice.");
                });

            ui.add_space(10.0);
            ui.collapsing("Manual conversion", |ui| {

            let drop_fill = if hovering {
                ui.visuals().selection.bg_fill
            } else {
                ui.visuals().faint_bg_color
            };
            egui::Frame::new()
                .fill(drop_fill)
                .stroke(ui.visuals().widgets.noninteractive.bg_stroke)
                .corner_radius(8)
                .inner_margin(18)
                .show(ui, |ui| {
                    ui.set_min_width(ui.available_width());
                    ui.vertical_centered(|ui| {
                        ui.strong(if hovering {
                            "Drop X3F files now"
                        } else {
                            "Drag X3F files into this window"
                        });
                        ui.label("or");
                        if ui
                            .add_enabled(!busy, egui::Button::new("Choose X3F files…"))
                            .clicked()
                        {
                            self.choose_files();
                        }
                    });
                });

            ui.add_space(10.0);
            ui.add_enabled_ui(!busy, |ui| {
                ui.strong("Export DNG files");
                ui.radio_value(
                    &mut self.destination,
                    Destination::BesideOriginals,
                    "Beside each original X3F",
                );
                ui.horizontal(|ui| {
                    ui.radio_value(
                        &mut self.destination,
                        Destination::ChosenDirectory,
                        "Into one chosen directory",
                    );
                    if ui.button("Choose…").clicked() {
                        self.choose_output_directory();
                    }
                });
                if self.destination == Destination::ChosenDirectory {
                    ui.label(
                        self.output_directory
                            .as_ref()
                            .map(|path| path.display().to_string())
                            .unwrap_or_else(|| "No directory selected".into()),
                    );
                }
                ui.checkbox(&mut self.overwrite, "Replace existing DNG files");
            });

            ui.separator();
            ui.horizontal(|ui| {
                ui.strong(format!("Files ({})", self.inputs.len()));
                if ui
                    .add_enabled(!busy && !self.inputs.is_empty(), egui::Button::new("Clear"))
                    .clicked()
                {
                    self.inputs.clear();
                    self.completed.clear();
                    self.notice = None;
                }
            });
            egui::ScrollArea::vertical()
                .max_height(120.0)
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    let mut remove = None;
                    for (index, path) in self.inputs.iter().enumerate() {
                        ui.horizontal(|ui| {
                            ui.label(
                                path.file_name()
                                    .unwrap_or(path.as_os_str())
                                    .to_string_lossy(),
                            )
                            .on_hover_text(path.display().to_string());
                            if !busy && ui.small_button("Remove").clicked() {
                                remove = Some(index);
                            }
                        });
                    }
                    if let Some(index) = remove {
                        self.inputs.remove(index);
                    }
                });

            if busy {
                let fraction = if self.total_count == 0 {
                    0.0
                } else {
                    self.completed_count as f32 / self.total_count as f32
                };
                ui.add(
                    egui::ProgressBar::new(fraction)
                        .show_percentage()
                        .text(format!(
                            "Converting {} of {}",
                            (self.completed_count + 1).min(self.total_count),
                            self.total_count
                        )),
                );
                if let Some(path) = &self.current_file {
                    ui.label(format!("Working on {}", path.display()));
                }
            } else if !self.completed.is_empty() {
                let failures = self
                    .completed
                    .iter()
                    .filter(|job| job.result.is_err())
                    .count();
                if failures == 0 {
                    ui.colored_label(
                        egui::Color32::from_rgb(35, 140, 75),
                        format!("Finished {} file(s).", self.completed.len()),
                    );
                } else {
                    ui.colored_label(
                        ui.visuals().error_fg_color,
                        format!("Finished with {failures} error(s)."),
                    );
                }
                egui::ScrollArea::vertical()
                    .max_height(90.0)
                    .show(ui, |ui| {
                        for job in &self.completed {
                            match &job.result {
                                Ok(report) => ui.label(format!(
                                    "✓ {} → {} ({:.1}s)",
                                    job.input.display(),
                                    report.output.display(),
                                    report.elapsed.as_secs_f64()
                                )),
                                Err(error) => ui.colored_label(
                                    ui.visuals().error_fg_color,
                                    format!("✗ {}: {error}", job.input.display()),
                                ),
                            };
                        }
                    });
            }

            ui.add_space(8.0);
            if ui
                .add_enabled(
                    !busy && !self.inputs.is_empty(),
                    egui::Button::new("Convert to DNG").min_size(egui::vec2(150.0, 34.0)),
                )
                .clicked()
            {
                self.start();
            }
            });
            if let Some(notice) = &self.notice {
                ui.colored_label(ui.visuals().warn_fg_color, notice);
            }
            ui.small("Compressed Merrill processing · no Lightroom Develop XMP embedded");
        });

        if busy || self.auto_importer.is_some() {
            context.request_repaint_after(Duration::from_millis(100));
        }
    }
}

fn main() {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([680.0, 620.0])
            .with_min_inner_size([520.0, 480.0])
            .with_drag_and_drop(true),
        ..Default::default()
    };
    if let Err(error) = eframe::run_native(
        "Fast Sigma Raw",
        options,
        Box::new(|_creation_context| Ok(Box::new(ConverterApp::new()))),
    ) {
        let _ = rfd::MessageDialog::new()
            .set_title("Fast Sigma Raw could not start")
            .set_description(error.to_string())
            .set_level(rfd::MessageLevel::Error)
            .set_buttons(rfd::MessageButtons::Ok)
            .show();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temporary_directory(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!("fast-sigma-raw-gui-{name}-{}", std::process::id()))
    }

    #[test]
    fn recognizes_only_sigma_dcim_directory_names() {
        assert!(is_sigma_dcim_directory("373SIGMA"));
        assert!(is_sigma_dcim_directory("100sigma"));
        assert!(!is_sigma_dcim_directory("SIGMA"));
        assert!(!is_sigma_dcim_directory("DCIM"));
        assert!(!is_sigma_dcim_directory("100MSDCF"));
    }

    #[test]
    fn older_settings_enable_safe_ejection_by_default() {
        let settings: AppSettings =
            serde_json::from_str(r#"{"automatic_import":true,"import_directory":"/tmp/imports"}"#)
                .unwrap();
        assert!(settings.eject_after_import);
    }

    #[test]
    fn card_scan_is_shallow_and_sigma_specific() {
        let volume = temporary_directory("card-layout");
        let sigma = volume.join("DCIM/373SIGMA");
        let unrelated = volume.join("DCIM/100MSDCF");
        let nested = sigma.join("nested");
        fs::create_dir_all(&unrelated).unwrap();
        fs::create_dir_all(&nested).unwrap();
        fs::write(sigma.join("DP2M0001.X3F"), b"x3f").unwrap();
        fs::write(unrelated.join("OTHER.X3F"), b"x3f").unwrap();
        fs::write(nested.join("NESTED.X3F"), b"x3f").unwrap();

        assert_eq!(
            sigma_card_x3f_files(&volume),
            vec![sigma.join("DP2M0001.X3F")]
        );

        fs::remove_dir_all(volume).unwrap();
    }
}
