use std::{
    collections::VecDeque,
    fs,
    io,
    path::{Component, Path, PathBuf},
};

fn invalid(path: &Path) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, format!("invalid native runtime link: {}", path.display()))
}

// Resolve each component before continuing: canonicalize alone would permit an
// outside-root intermediate link that eventually points back into the install.
fn resolve(root: &Path, path: &Path) -> io::Result<PathBuf> {
    let mut pending = components(path.strip_prefix(root).map_err(|_| invalid(path))?)?;
    let mut resolved = root.to_path_buf();
    let mut links = 0;
    while let Some(part) = pending.pop_front() {
        resolved.push(part);
        let kind = fs::symlink_metadata(&resolved)?.file_type();
        if kind.is_symlink() {
            links += 1;
            if links > 32 {
                return Err(invalid(path));
            }
            let target = fs::read_link(&resolved)?;
            resolved.pop();
            let target = if target.is_absolute() {
                resolved = root.to_path_buf();
                target.strip_prefix(root).map_err(|_| invalid(path))?
            } else {
                target.as_path()
            };
            let mut next = components(target)?;
            next.append(&mut pending);
            pending = next;
        } else if !kind.is_file() && !kind.is_dir() {
            return Err(invalid(path));
        }
    }
    if !fs::symlink_metadata(&resolved)?.file_type().is_file() {
        return Err(invalid(path));
    }
    Ok(resolved)
}

fn components(path: &Path) -> io::Result<VecDeque<std::ffi::OsString>> {
    if path.as_os_str().is_empty() {
        return Err(invalid(path));
    }
    path.components().map(|part| match part {
        Component::Normal(name) => Ok(name.to_os_string()),
        _ => Err(invalid(path)),
    }).collect()
}

pub fn materialize_runtime(stage: &Path) -> io::Result<()> {
    let root = fs::canonicalize(stage)?;
    for (index, entry) in fs::read_dir(&root)?.enumerate() {
        let entry = entry?;
        if !entry.file_type()?.is_symlink() {
            continue;
        }
        let source = resolve(&root, &entry.path())?;
        // Copy before replacing the link so failed copies leave the install
        // intact. create_new prevents overwriting an existing staging input.
        let temporary = root.join(format!(".shinki-runtime-{}-{index}", std::process::id()));
        let file = fs::OpenOptions::new().write(true).create_new(true).open(&temporary)?;
        drop(file);
        let result = fs::copy(&source, &temporary).and_then(|_| fs::rename(&temporary, entry.path()));
        if result.is_err() {
            let _ = fs::remove_file(&temporary);
        }
        result?;
    }
    Ok(())
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::{os::unix::fs::{symlink, FileTypeExt}, sync::atomic::{AtomicUsize, Ordering}};

    static NEXT: AtomicUsize = AtomicUsize::new(0);
    struct Install(PathBuf);
    impl Install {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!("impacto-runtime-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
            fs::create_dir(&path).unwrap();
            Self(path)
        }
    }
    impl Drop for Install {
        fn drop(&mut self) { fs::remove_dir_all(&self.0).unwrap(); }
    }

    #[test]
    fn soname_chain_becomes_identical_regular_files() {
        let install = Install::new();
        let bytes = b"\x7fELF\0runtime-library\xff";
        fs::write(install.0.join("libSDL3.so.0.2"), bytes).unwrap();
        symlink("libSDL3.so.0.2", install.0.join("libSDL3.so.0")).unwrap();
        symlink("libSDL3.so.0", install.0.join("libSDL3.so")).unwrap();
        materialize_runtime(&install.0).unwrap();
        for name in ["libSDL3.so.0.2", "libSDL3.so.0", "libSDL3.so"] {
            let path = install.0.join(name);
            assert!(fs::symlink_metadata(&path).unwrap().file_type().is_file());
            assert_eq!(fs::read(path).unwrap(), bytes);
        }
    }

    #[test]
    fn rejects_dangling_cycle_directory_and_parent_paths() {
        for target in ["missing", "link", ".", "../outside"] {
            let install = Install::new();
            symlink(target, install.0.join("link")).unwrap();
            assert!(materialize_runtime(&install.0).is_err(), "{target}");
            assert!(fs::symlink_metadata(install.0.join("link")).unwrap().file_type().is_symlink());
        }
    }

    #[test]
    fn rejects_outside_root_even_when_link_returns_inside() {
        let install = Install::new();
        let outside = Install::new();
        fs::write(install.0.join("library"), b"inside").unwrap();
        symlink(install.0.join("library"), outside.0.join("return")).unwrap();
        symlink(outside.0.join("return"), install.0.join("link")).unwrap();
        assert!(materialize_runtime(&install.0).is_err());
    }

    #[test]
    fn rejects_special_file_target() {
        let install = Install::new();
        let socket = install.0.join("socket");
        let _listener = std::os::unix::net::UnixListener::bind(&socket).unwrap();
        assert!(fs::symlink_metadata(&socket).unwrap().file_type().is_socket());
        symlink("socket", install.0.join("link")).unwrap();
        assert!(materialize_runtime(&install.0).is_err());
    }
}
