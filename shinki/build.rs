use shinki_emu_build::{
    configure_cmake, generate_import_library, write_core_build_manifest, BuildAsset, CoreBuildManifest,
};
use std::{
    env,
    error::Error,
    fs,
    path::{Path, PathBuf},
};

type Result<T, E = Box<dyn Error>> = std::result::Result<T, E>;

fn required_dir(name: &str) -> Result<PathBuf> {
    let path = PathBuf::from(env::var_os(name).ok_or_else(|| {
        format!("{name} is required; run cargo xtask deps for the selected Impacto target")
    })?);
    if !path.is_dir() {
        return Err(format!("{name} directory is missing: {}", path.display()).into());
    }
    Ok(std::path::absolute(path)?)
}

fn require_file(path: &Path) -> Result<()> {
    if !path.is_file() {
        return Err(format!("missing Impacto build prerequisite: {}; run cargo xtask deps for the selected target", path.display()).into());
    }
    Ok(())
}

fn copy_tree(source: &Path, dest: &Path) -> Result<()> {
    fs::create_dir_all(dest)?;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let kind = entry.file_type()?;
        let output = dest.join(entry.file_name());
        if kind.is_dir() {
            copy_tree(&entry.path(), &output)?;
        } else if kind.is_file() {
            fs::copy(entry.path(), output)?;
        } else {
            return Err(format!(
                "dependency source contains a link or special file: {}",
                entry.path().display()
            )
            .into());
        }
    }
    Ok(())
}

fn build() -> Result<()> {
    for name in [
        "SHINKI_EMU_NATIVE_DOWNLOADS",
        "VCPKG_ROOT",
        "SHINKI_EMU_VCPKG_INSTALLED",
        "SHINKI_EMU_VCPKG_TRIPLET",
        "SHINKI_EMU_FFMPEG_INSTALLED",
        "NUM_JOBS",
        "PROFILE",
        "HOST",
        "TARGET",
    ] {
        println!("cargo:rerun-if-env-changed={name}");
    }
    let target = env::var("TARGET")?;
    let host = env::var("HOST")?;
    let supported = matches!(
        target.as_str(),
        "x86_64-pc-windows-msvc"
            | "x86_64-unknown-linux-gnu"
            | "x86_64-apple-darwin"
            | "aarch64-apple-darwin"
            | "aarch64-apple-ios"
            | "aarch64-apple-ios-sim"
            | "x86_64-apple-ios"
            | "aarch64-linux-android"
            | "x86_64-linux-android"
    );
    if !supported {
        return Err(format!("Impacto's embedded adapter does not support HOST={host}, TARGET={target}").into());
    }
    // configure_cmake reads NUM_JOBS. Cap the native build even when Cargo has
    // been started outside xtask with a higher job count.
    let jobs = env::var("NUM_JOBS")
        .ok()
        .and_then(|v| v.parse::<usize>().ok())
        .unwrap_or(4)
        .clamp(1, 4);
    unsafe { env::set_var("NUM_JOBS", jobs.to_string()) };

    let adapter = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").ok_or("CARGO_MANIFEST_DIR is required")?);
    let source = std::path::absolute(adapter.parent().ok_or("adapter must be in the Impacto checkout")?)?;
    let vcpkg = required_dir("VCPKG_ROOT")?;
    let installed = required_dir("SHINKI_EMU_VCPKG_INSTALLED")?;
    let triplet = env::var("SHINKI_EMU_VCPKG_TRIPLET")?;
    let expected_triplet = match target.as_str() {
        "x86_64-pc-windows-msvc" => "x64-windows",
        "x86_64-unknown-linux-gnu" => "x64-linux-dynamic",
        "x86_64-apple-darwin" => "x64-osx",
        "aarch64-apple-darwin" => "arm64-osx",
        "aarch64-apple-ios" => "arm64-ios",
        "aarch64-apple-ios-sim" => "arm64-ios-simulator",
        "x86_64-apple-ios" => "x64-ios",
        "aarch64-linux-android" => "arm64-android-ffmpeg",
        "x86_64-linux-android" => "x64-android-ffmpeg",
        _ => unreachable!(),
    };
    if triplet != expected_triplet {
        return Err(format!("Impacto requires the {expected_triplet} dependency triplet, got {triplet}").into());
    }
    if let Some(raw) = env::var_os("SHINKI_EMU_FFMPEG_INSTALLED") {
        let ffmpeg = PathBuf::from(raw);
        if !ffmpeg.is_dir() {
            return Err(format!("SHINKI_EMU_FFMPEG_INSTALLED directory is missing: {}", ffmpeg.display()).into());
        }
        let mut prefixes = vec![installed.join(&triplet), ffmpeg.clone()];
        if let Some(existing) = env::var_os("CMAKE_PREFIX_PATH") {
            prefixes.extend(env::split_paths(&existing));
        }
        unsafe { env::set_var("CMAKE_PREFIX_PATH", env::join_paths(prefixes)?); }
        let mut pkgconfig = vec![
            installed.join(&triplet).join("lib/pkgconfig"),
            installed.join(&triplet).join("share/pkgconfig"),
            ffmpeg.join("lib/pkgconfig"),
            ffmpeg.join("share/pkgconfig"),
        ];
        if let Some(existing) = env::var_os("PKG_CONFIG_PATH") {
            pkgconfig.extend(env::split_paths(&existing));
        }
        unsafe { env::set_var("PKG_CONFIG_PATH", env::join_paths(pkgconfig)?); }
    }
    let toolchain = vcpkg.join("scripts/buildsystems/vcpkg.cmake");
    require_file(&toolchain)?;
    println!("cargo:rerun-if-changed={}", toolchain.display());
    for path in ["include", "lib", "bin", "debug/lib", "debug/bin", "share"] {
        let input = installed.join(&triplet).join(path);
        if input.exists() {
            println!("cargo:rerun-if-changed={}", input.display());
        }
    }
    let share = installed.join(&triplet).join("share");
    let ffmpeg_installed = env::var_os("SHINKI_EMU_FFMPEG_INSTALLED").map(PathBuf::from);
    for port in ["sdl3", "avcpp", "openal-soft", "libvorbis", "libwebp", "freetype", "harfbuzz"] {
        require_file(&share.join(port).join("copyright"))?;
    }
    if ffmpeg_installed.is_none() {
        require_file(&share.join("ffmpeg").join("copyright"))?;
    }
    let downloads = required_dir("SHINKI_EMU_NATIVE_DOWNLOADS")?;
    let atrac_source = downloads.join("libatrac9");
    let magic = downloads.join("magic_enum");
    require_file(&atrac_source.join("C/src/libatrac9.c"))?;
    require_file(&magic.join("CMakeLists.txt"))?;
    require_file(&magic.join("include/magic_enum/magic_enum.hpp"))?;
    println!("cargo:rerun-if-changed={}", atrac_source.display());
    println!("cargo:rerun-if-changed={}", magic.display());
    let atrac_patch = source.join("vendor/patches/LibAtrac9/CMakeLists.txt");
    require_file(&atrac_patch)?;
    println!("cargo:rerun-if-changed={}", atrac_patch.display());
    let atrac = PathBuf::from(env::var_os("OUT_DIR").ok_or("OUT_DIR is required")?)
        .join("src/libatrac9");
    if atrac.exists() {
        fs::remove_dir_all(&atrac)?;
    }
    copy_tree(&atrac_source, &atrac)?;
    fs::copy(atrac_patch, atrac.join("CMakeLists.txt"))?;

    let definitions = vec![
        (
            "CMAKE_TOOLCHAIN_FILE".into(),
            toolchain.to_string_lossy().into_owned(),
        ),
        ("IMPACTO_SHINKI_EMU".into(), "ON".into()),
        (
            "IMPACTO_SHINKI_EMU_STATIC".into(),
            if cfg!(feature = "static") { "ON" } else { "OFF" }.into(),
        ),
        (
            "SHINKI_EMU_ABI_INCLUDE_DIR".into(),
            source.join("vendor/shinki-emu-abi/include").to_string_lossy().into_owned(),
        ),
        (
            "FETCHCONTENT_SOURCE_DIR_LIBATRAC9".into(),
            atrac.to_string_lossy().into_owned(),
        ),
        (
            "FETCHCONTENT_SOURCE_DIR_MAGIC_ENUM".into(),
            magic.to_string_lossy().into_owned(),
        ),
        ("CMAKE_INSTALL_BINDIR".into(), ".".into()),
        ("CMAKE_INSTALL_LIBDIR".into(), "lib".into()),
        ("CMAKE_INSTALL_INCLUDEDIR".into(), "include".into()),
    ];
    let native = configure_cmake(&source, &definitions, &["install"])?;
    let stage = native.join("install");
    let library = if cfg!(feature = "static") {
        let name = if target == "x86_64-pc-windows-msvc" {
            "shinki_emu_impacto.lib"
        } else {
            "libshinki_emu_impacto.a"
        };
        stage.join("lib").join(name)
    } else {
        let name = if target == "x86_64-pc-windows-msvc" {
            "shinki_emu_impacto.dll"
        } else if target == "x86_64-unknown-linux-gnu" || target.ends_with("-linux-android") {
            "shinki_emu_impacto.so"
        } else {
            "shinki_emu_impacto.dylib"
        };
        stage.join(name)
    };
    require_file(&library)?;
    let resources = stage.join("Resources");
    for path in ["profiles/cc/game.lua", "src/shaders", "resources"] {
        if !resources.join(path).exists() {
            return Err(format!("native Impacto install omitted Resources/{path}").into());
        }
    }
    println!("cargo:rustc-link-search=native={}", stage.join("lib").display());
    if cfg!(feature = "static") {
        println!("cargo:rustc-link-lib=static=shinki_emu_impacto");
    } else {
        println!("cargo:rustc-link-search=native={}", stage.display());
        if target == "x86_64-pc-windows-msvc" {
            let import = generate_import_library(&library, &native.join("imports"), &target)?;
            println!(
                "cargo:rustc-link-search=native={}",
                import.parent().ok_or("import library parent")?.display()
            );
        }
        println!("cargo:rustc-link-lib=dylib=shinki_emu_impacto");
    }

    let mut manifest = CoreBuildManifest::new("impacto", "native-shared")?;
    manifest.native_library = Some(library);
    manifest.resources.push(BuildAsset {
        source: resources,
        dest: "Resources".into(),
    });
    manifest.licenses.push(BuildAsset {
        source: source.join("LICENSE"),
        dest: "licenses/impacto/LICENSE".into(),
    });
    manifest.licenses.push(BuildAsset {
        source: source.join("THIRDPARTY.md"),
        dest: "licenses/impacto/THIRDPARTY.md".into(),
    });
    for (path, name) in [
        ("README.md", "README.md"),
        ("licenses/NotoSansCJK-OFL.txt", "NotoSansCJK-OFL.txt"),
    ] {
        manifest.licenses.push(BuildAsset {
            source: source.join(path),
            dest: format!("licenses/impacto/{name}"),
        });
    }
    // vcpkg's per-port notices include both DLL and header/static dependencies.
    // Keep the full prepared target installation's notices to cover transitive
    // dependencies without guessing their filenames or linkage.
    let mut ports = fs::read_dir(&share)?.collect::<std::io::Result<Vec<_>>>()?;
    ports.sort_by_key(|entry| entry.file_name());
    for port in ports {
        if !port.file_type()?.is_dir() {
            continue;
        }
        let copyright = port.path().join("copyright");
        if copyright.is_file() {
            let name = port
                .file_name()
                .into_string()
                .map_err(|_| "non-UTF-8 vcpkg port name")?;
            manifest.licenses.push(BuildAsset {
                source: copyright,
                dest: format!("licenses/impacto/vcpkg/{name}/copyright"),
            });
        }
    }
    if let Some(ffmpeg) = ffmpeg_installed {
        let ffmpeg_share = ffmpeg.join("share");
        let mut ffmpeg_ports = fs::read_dir(&ffmpeg_share)?.collect::<std::io::Result<Vec<_>>>()?;
        ffmpeg_ports.sort_by_key(|entry| entry.file_name());
        for port in ffmpeg_ports {
            if !port.file_type()?.is_dir() {
                continue;
            }
            let copyright = port.path().join("copyright");
            if copyright.is_file() {
                let name = port
                    .file_name()
                    .into_string()
                    .map_err(|_| "non-UTF-8 FFmpeg port name")?;
                manifest.licenses.push(BuildAsset {
                    source: copyright,
                    dest: format!("licenses/impacto/vcpkg/ffmpeg/{name}/copyright"),
                });
            }
        }
    }
    write_core_build_manifest(&manifest)?;
    Ok(())
}

fn main() {
    if let Err(error) = build() {
        panic!("Impacto native build failed: {error}");
    }
}
