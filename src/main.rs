use std::env;
use std::path::PathBuf;
use std::process::ExitCode;

use fast_sigma_raw::{ConversionOptions, Pipeline, SpatialGain, convert};

const HELP: &str = "\
fast-sigma-raw — convert SIGMA/Foveon X3F to linear DNG

USAGE:
    fast-sigma-raw [OPTIONS] <INPUT.X3F> [OUTPUT.DNG]

OPTIONS:
    -f, --force                 overwrite an existing output
        --no-compress           store uncompressed 16-bit samples
        --no-fix-bad-pixels     preserve marked bad pixels
        --spatial-gain <MODE>   auto, on, or off [default: auto]
        --pipeline <MODE>       merrill, raw, or bmt [default: merrill]
    -h, --help                  print this help
    -V, --version               print the version
";

#[derive(Debug)]
struct Options {
    input: PathBuf,
    output: PathBuf,
    compress: bool,
    fix_bad_pixels: bool,
    spatial_gain: SpatialGain,
    force: bool,
    pipeline: Pipeline,
}

fn parse_args() -> Result<Option<Options>, String> {
    let mut args = env::args_os().skip(1).peekable();
    let mut positional = Vec::new();
    let mut compress = true;
    let mut fix_bad_pixels = true;
    let mut spatial_gain = SpatialGain::Auto;
    let mut force = false;
    let mut pipeline = Pipeline::Merrill;

    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("-h" | "--help") => {
                print!("{HELP}");
                return Ok(None);
            }
            Some("-V" | "--version") => {
                println!("fast-sigma-raw {}", env!("CARGO_PKG_VERSION"));
                return Ok(None);
            }
            Some("-f" | "--force") => force = true,
            Some("--no-compress") => compress = false,
            Some("--no-fix-bad-pixels") => fix_bad_pixels = false,
            Some("--spatial-gain") => {
                let mode = args
                    .next()
                    .ok_or("--spatial-gain requires auto, on, or off")?;
                spatial_gain = match mode.to_str() {
                    Some("auto") => SpatialGain::Auto,
                    Some("on") => SpatialGain::On,
                    Some("off") => SpatialGain::Off,
                    _ => return Err("--spatial-gain requires auto, on, or off".into()),
                };
            }
            Some("--pipeline") => {
                let mode = args
                    .next()
                    .ok_or("--pipeline requires merrill, raw, or bmt")?;
                pipeline = match mode.to_str() {
                    Some("merrill") => Pipeline::Merrill,
                    Some("raw") => Pipeline::Raw,
                    Some("bmt") => Pipeline::Bmt,
                    _ => return Err("--pipeline requires merrill, raw, or bmt".into()),
                };
            }
            Some(value) if value.starts_with('-') => {
                return Err(format!("unknown option: {value}"));
            }
            _ => positional.push(PathBuf::from(arg)),
        }
    }

    if positional.is_empty() || positional.len() > 2 {
        return Err("expected INPUT.X3F and optional OUTPUT.DNG".into());
    }
    let input = positional.remove(0);
    let output = if positional.is_empty() {
        input.with_extension("dng")
    } else {
        positional.remove(0)
    };

    Ok(Some(Options {
        input,
        output,
        compress,
        fix_bad_pixels,
        spatial_gain,
        force,
        pipeline,
    }))
}

fn run(options: Options) -> Result<(), String> {
    let report = convert(
        &options.input,
        &options.output,
        ConversionOptions {
            compress: options.compress,
            fix_bad_pixels: options.fix_bad_pixels,
            spatial_gain: options.spatial_gain,
            pipeline: options.pipeline,
            overwrite: options.force,
        },
    )?;
    let size_mb = report.bytes as f64 / (1024.0 * 1024.0);
    eprintln!(
        "wrote {} ({size_mb:.1} MiB) in {:.2}s",
        report.output.display(),
        report.elapsed.as_secs_f64()
    );
    Ok(())
}

fn main() -> ExitCode {
    match parse_args().and_then(|options| options.map_or(Ok(()), run)) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error}\n\nRun with --help for usage.");
            ExitCode::FAILURE
        }
    }
}
