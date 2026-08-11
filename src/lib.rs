use std::ffi::{CStr, CString, c_char, c_int};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

unsafe extern "C" {
    fn fsr_convert(
        input: *const c_char,
        output: *const c_char,
        compress: c_int,
        fix_bad_pixels: c_int,
        spatial_gain_mode: c_int,
        pipeline: c_int,
    ) -> c_int;
    fn fsr_error_string(code: c_int) -> *const c_char;
}

static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SpatialGain {
    Auto,
    On,
    Off,
}

impl SpatialGain {
    fn native_value(self) -> c_int {
        match self {
            Self::Auto => -1,
            Self::On => 1,
            Self::Off => 0,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Pipeline {
    Merrill,
    Raw,
    Bmt,
}

impl Pipeline {
    fn native_value(self) -> c_int {
        match self {
            Self::Merrill => 1,
            Self::Raw => 0,
            Self::Bmt => 2,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ConversionOptions {
    pub compress: bool,
    pub fix_bad_pixels: bool,
    pub spatial_gain: SpatialGain,
    pub pipeline: Pipeline,
    pub overwrite: bool,
}

impl Default for ConversionOptions {
    fn default() -> Self {
        Self {
            compress: true,
            fix_bad_pixels: true,
            spatial_gain: SpatialGain::Auto,
            pipeline: Pipeline::Merrill,
            overwrite: false,
        }
    }
}

#[derive(Debug)]
pub struct ConversionReport {
    pub output: PathBuf,
    pub bytes: u64,
    pub elapsed: Duration,
}

pub fn is_x3f(path: &Path) -> bool {
    path.is_file()
        && path
            .extension()
            .is_some_and(|extension| extension.eq_ignore_ascii_case("x3f"))
}

pub fn default_output_path(input: &Path, output_directory: Option<&Path>) -> PathBuf {
    let name = input
        .file_stem()
        .map(|stem| {
            let mut name = stem.to_os_string();
            name.push(".dng");
            name
        })
        .unwrap_or_else(|| "output.dng".into());
    output_directory
        .map(|directory| directory.join(&name))
        .unwrap_or_else(|| input.with_file_name(name))
}

fn c_path(path: &Path) -> Result<CString, String> {
    CString::new(path.as_os_str().as_encoded_bytes())
        .map_err(|_| format!("path contains a NUL byte: {}", path.display()))
}

fn sibling_temporary(path: &Path, purpose: &str) -> PathBuf {
    let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    path.with_file_name(format!(
        ".fast-sigma-raw-{}-{sequence}-{purpose}.tmp",
        std::process::id()
    ))
}

fn publish(temporary: &Path, output: &Path, overwrite: bool) -> Result<(), String> {
    if !output.exists() {
        return fs::rename(temporary, output)
            .map_err(|error| format!("cannot publish {}: {error}", output.display()));
    }
    if !overwrite {
        return Err(format!("output already exists: {}", output.display()));
    }

    // Windows cannot atomically rename over an existing file. Keep the old
    // DNG beside the destination until the replacement has been published so
    // a failed rename never destroys the user's prior export.
    let backup = sibling_temporary(output, "backup");
    fs::rename(output, &backup).map_err(|error| {
        format!(
            "cannot prepare replacement for {}: {error}",
            output.display()
        )
    })?;
    match fs::rename(temporary, output) {
        Ok(()) => {
            let _ = fs::remove_file(backup);
            Ok(())
        }
        Err(error) => {
            let restore = fs::rename(&backup, output);
            let suffix = restore
                .err()
                .map(|restore_error| {
                    format!("; restoring the old file also failed: {restore_error}")
                })
                .unwrap_or_default();
            Err(format!(
                "cannot publish {}: {error}{suffix}",
                output.display()
            ))
        }
    }
}

pub fn convert(
    input: &Path,
    output: &Path,
    options: ConversionOptions,
) -> Result<ConversionReport, String> {
    if !input.is_file() {
        return Err(format!("input does not exist: {}", input.display()));
    }
    if !input
        .extension()
        .is_some_and(|extension| extension.eq_ignore_ascii_case("x3f"))
    {
        return Err(format!("input is not an X3F file: {}", input.display()));
    }
    if output.exists() && !options.overwrite {
        return Err(format!("output already exists: {}", output.display()));
    }
    if let Some(parent) = output.parent()
        && !parent.as_os_str().is_empty()
        && !parent.is_dir()
    {
        return Err(format!(
            "output directory does not exist: {}",
            parent.display()
        ));
    }

    let temporary = sibling_temporary(output, "conversion");
    let input_c = c_path(input)?;
    let output_c = c_path(&temporary)?;
    let start = Instant::now();
    // SAFETY: both C strings live for the duration of the call, and the bridge
    // does not retain their pointers.
    let code = unsafe {
        fsr_convert(
            input_c.as_ptr(),
            output_c.as_ptr(),
            options.compress as c_int,
            options.fix_bad_pixels as c_int,
            options.spatial_gain.native_value(),
            options.pipeline.native_value(),
        )
    };
    if code != 0 {
        let _ = fs::remove_file(&temporary);
        // SAFETY: the bridge returns a static NUL-terminated string.
        let message = unsafe { CStr::from_ptr(fsr_error_string(code)) }.to_string_lossy();
        return Err(format!("conversion failed: {message} (code {code})"));
    }

    if let Err(error) = publish(&temporary, output, options.overwrite) {
        let _ = fs::remove_file(&temporary);
        return Err(error);
    }
    Ok(ConversionReport {
        output: output.to_path_buf(),
        bytes: output
            .metadata()
            .map(|metadata| metadata.len())
            .unwrap_or(0),
        elapsed: start.elapsed(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn output_defaults_to_the_input_directory() {
        assert_eq!(
            default_output_path(Path::new("photos/example.X3F"), None),
            Path::new("photos/example.dng")
        );
    }

    #[test]
    fn output_can_use_a_chosen_directory() {
        assert_eq!(
            default_output_path(Path::new("photos/example.x3f"), Some(Path::new("exports"))),
            Path::new("exports/example.dng")
        );
    }

    #[test]
    fn publish_replaces_without_deleting_the_old_file_early() {
        let directory = std::env::temp_dir().join(format!(
            "fast-sigma-raw-publish-test-{}-{}",
            std::process::id(),
            TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&directory).unwrap();
        let output = directory.join("image.dng");
        let temporary = directory.join("new.tmp");
        fs::write(&output, b"old").unwrap();
        fs::write(&temporary, b"new").unwrap();

        publish(&temporary, &output, true).unwrap();

        assert_eq!(fs::read(&output).unwrap(), b"new");
        assert!(!temporary.exists());
        fs::remove_dir_all(directory).unwrap();
    }
}
