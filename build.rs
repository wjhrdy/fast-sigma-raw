fn main() {
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let static_tiff = std::env::var_os("LIBTIFF_STATIC").is_some();
    let manual_linux_static = static_tiff && target_os == "linux";
    let mut tiff_config = pkg_config::Config::new();
    tiff_config
        .atleast_version("4.0")
        .statik(static_tiff)
        .cargo_metadata(!manual_linux_static);
    let tiff = tiff_config
        .probe("libtiff-4")
        .expect("libtiff is required (macOS: brew install libtiff; Linux: install libtiff-dev)");
    if manual_linux_static {
        for path in &tiff.link_paths {
            println!("cargo:rustc-link-search=native={}", path.display());
        }
        for library in &tiff.libs {
            if matches!(library.as_str(), "c" | "dl" | "m" | "pthread" | "rt") {
                println!("cargo:rustc-link-lib={library}");
            } else {
                println!("cargo:rustc-link-lib=static={library}");
            }
        }
    }
    let sources = [
        "vendor/x3f-tools/x3f_io.c",
        "vendor/x3f-tools/x3f_process.c",
        "vendor/x3f-tools/x3f_meta.c",
        "vendor/x3f-tools/x3f_image.c",
        "vendor/x3f-tools/x3f_spatial_gain.c",
        "vendor/x3f-tools/x3f_output_dng.c",
        "vendor/x3f-tools/x3f_matrix.c",
        "vendor/x3f-tools/x3f_dngtags.c",
        "vendor/x3f-tools/x3f_printf.c",
        "vendor/x3f-tools/fast_sigma_shim.c",
    ];

    let mut build = cc::Build::new();
    let package_version = std::env::var("CARGO_PKG_VERSION").unwrap_or_default();
    let version_define = format!("\"{package_version}\"");
    build
        .files(sources)
        .include("vendor/x3f-tools")
        .includes(&tiff.include_paths)
        .opt_level(3)
        .warnings(false)
        .define("FAST_SIGMA_RAW_VERSION", version_define.as_str())
        .flag_if_supported("-ffast-math")
        .flag_if_supported("-fno-math-errno")
        .compile("fast_sigma_x3f");

    for path in sources {
        println!("cargo:rerun-if-changed={path}");
    }
    println!("cargo:rerun-if-changed=vendor/x3f-tools/x3f_denoise.h");
    println!("cargo:rerun-if-env-changed=LIBTIFF_STATIC");
    println!("cargo:rerun-if-env-changed=CARGO_PKG_VERSION");
    #[cfg(target_os = "macos")]
    println!("cargo:rustc-link-lib=iconv");
    if target_os == "linux" {
        println!("cargo:rustc-link-lib=pthread");
    }
}
