fn main() {
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let target_env = std::env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();
    let static_tiff = std::env::var_os("LIBTIFF_STATIC").is_some();
    let tiff = pkg_config::Config::new()
        .atleast_version("4.0")
        .statik(static_tiff)
        .probe("libtiff-4")
        .expect("libtiff is required (macOS: brew install libtiff; Linux: install libtiff-dev)");
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
    build
        .files(sources)
        .include("vendor/x3f-tools")
        .includes(&tiff.include_paths)
        .opt_level(3)
        .warnings(false)
        .flag_if_supported("-ffast-math")
        .flag_if_supported("-fno-math-errno")
        .compile("fast_sigma_x3f");

    for path in sources {
        println!("cargo:rerun-if-changed={path}");
    }
    println!("cargo:rerun-if-changed=vendor/x3f-tools/x3f_denoise.h");
    println!("cargo:rerun-if-env-changed=LIBTIFF_STATIC");
    #[cfg(target_os = "macos")]
    println!("cargo:rustc-link-lib=iconv");
    if target_os == "linux" {
        println!("cargo:rustc-link-lib=pthread");
    } else if target_os == "windows" && target_env == "gnu" {
        println!("cargo:rustc-link-lib=winpthread");
    }

    if target_os == "windows" {
        winresource::WindowsResource::new()
            .set_manifest_file("packaging/windows/app.manifest")
            .compile()
            .expect("failed to compile the Windows application manifest");
        println!("cargo:rerun-if-changed=packaging/windows/app.manifest");
    }
}
