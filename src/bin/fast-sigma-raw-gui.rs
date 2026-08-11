use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver};
use std::time::Duration;

use eframe::egui;
use fast_sigma_raw::{ConversionOptions, ConversionReport, convert, default_output_path, is_x3f};

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

#[derive(Default)]
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
}

impl ConverterApp {
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
}

impl eframe::App for ConverterApp {
    fn ui(&mut self, root_ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let context = root_ui.ctx().clone();
        self.receive_updates();
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

            if let Some(notice) = &self.notice {
                ui.colored_label(ui.visuals().warn_fg_color, notice);
            }

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
            ui.small("Compressed Merrill processing · no Lightroom Develop XMP embedded");
        });

        if busy {
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
        Box::new(|_creation_context| Ok(Box::<ConverterApp>::default())),
    ) {
        let _ = rfd::MessageDialog::new()
            .set_title("Fast Sigma Raw could not start")
            .set_description(error.to_string())
            .set_level(rfd::MessageLevel::Error)
            .set_buttons(rfd::MessageButtons::Ok)
            .show();
    }
}
