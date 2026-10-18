use std::fs;
use std::path::{Path, PathBuf};

fn collect(root: &Path, directory: &Path, files: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(directory).expect("read tracked config directory") {
        let path = entry.expect("read tracked config entry").path();
        if path.is_dir() {
            collect(root, &path, files);
        } else if path.is_file() {
            assert!(path.starts_with(root));
            files.push(path);
        }
    }
}

fn main() {
    println!("cargo:rustc-check-cfg=cfg(kazarov_ffmpeg)");
    println!("cargo:rerun-if-env-changed=KAZAROV_FFMPEG_INCLUDE");
    println!("cargo:rerun-if-env-changed=KAZAROV_FFMPEG_LIB");
    println!("cargo:rerun-if-changed=src/c/movie_mux.c");
    if let Some(include) = std::env::var_os("KAZAROV_FFMPEG_INCLUDE") {
        cc::Build::new()
            .file("src/c/movie_mux.c")
            .include(include)
            .compile("kazarov_movie_mux");
        println!("cargo:rustc-cfg=kazarov_ffmpeg");
        for name in ["avformat", "avcodec", "avutil"] {
            println!("cargo:rustc-link-lib=dylib={name}");
        }
        if let Some(lib) = std::env::var_os("KAZAROV_FFMPEG_LIB") {
            println!(
                "cargo:rustc-link-search=native={}",
                PathBuf::from(lib).display()
            );
        }
    }
    let configured_root = std::env::var_os("KAZAROV_GAMEPLAY_ROOT");
    let root = configured_root
        .as_ref()
        .map(|value| PathBuf::from(value.as_os_str()))
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../port/gameplay"));
    println!("cargo:rerun-if-env-changed=KAZAROV_GAMEPLAY_ROOT");
    println!("cargo:rerun-if-changed={}", root.display());
    if configured_root.is_none() && !root.join("character_clips.json").is_file() {
        let out = PathBuf::from(std::env::var_os("OUT_DIR").expect("OUT_DIR"));
        fs::write(
            out.join("tracked_configs.rs"),
            "pub(crate) static TRACKED_GAMEPLAY_PATHS: &[&str] = &[];\n\
             pub(crate) static TRACKED_CONFIG_PATHS: &[&str] = &[];\n\
             pub(crate) static EMBEDDED_GAMEPLAY: &[(&str, usize, usize)] = &[];\n\
             pub(crate) const TRACKED_CONFIG_FINGERPRINT: &str = \"0000000000000000\";\n",
        )
        .expect("write empty gameplay index");
        fs::write(out.join("gameplay_documents.bin"), []).expect("write empty gameplay data");
        fs::write(
            out.join("character_clip_plan.json"),
            br#"{"schema_version":1,"kind":"character_clips","characters":{}}"#,
        )
        .expect("write empty character clip plan");
        return;
    }
    assert!(
        root.is_dir(),
        "tracked gameplay directory missing: {}",
        root.display()
    );
    let mut files = Vec::new();
    collect(&root, &root, &mut files);
    files.sort();
    let mut generated = String::from("pub(crate) static TRACKED_GAMEPLAY_PATHS: &[&str] = &[\n");
    let mut gameplay_paths = Vec::new();
    let mut embedded = Vec::new();
    let mut payload = Vec::new();
    let mut fingerprint: u64 = 0xcbf29ce484222325;
    for file in files {
        let relative = file
            .strip_prefix(&root)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        assert!(
            relative.ends_with(".json"),
            "non-JSON gameplay file: {relative}"
        );
        println!("cargo:rerun-if-changed={}", file.display());
        let bytes = fs::read(&file).expect("read tracked config");
        let json: serde_json::Value =
            serde_json::from_slice(&bytes).expect("parse tracked config JSON");
        let standalone = relative.ends_with(".cfg.json") || relative.ends_with(".map.json");
        assert!(
            json["schema_version"] == if standalone { 2 } else { 1 },
            "unsupported gameplay schema: {relative}"
        );
        assert!(
            json["kind"].is_string() && (!standalone || json.get("commands").is_none()),
            "invalid gameplay document: {relative}"
        );
        for byte in relative.bytes().chain([0]).chain(bytes.iter().copied()) {
            fingerprint ^= u64::from(byte);
            fingerprint = fingerprint.wrapping_mul(0x100000001b3);
        }
        let path = relative.to_ascii_lowercase();
        let offset = payload.len();
        let compact = serde_json::to_vec(&json).expect("serialize tracked config JSON");
        payload.extend_from_slice(&compact);
        embedded.push((path.clone(), offset, compact.len()));
        gameplay_paths.push(path);
    }
    gameplay_paths.sort();
    let before = gameplay_paths.len();
    gameplay_paths.dedup();
    assert_eq!(
        gameplay_paths.len(),
        before,
        "gameplay paths collide ignoring case"
    );
    for path in gameplay_paths {
        generated.push_str(&format!("    {path:?},\n"));
    }
    generated.push_str("];\n");
    embedded.sort_by(|a, b| a.0.cmp(&b.0));
    generated.push_str("pub(crate) static EMBEDDED_GAMEPLAY: &[(&str, usize, usize)] = &[\n");
    for (path, offset, len) in embedded {
        generated.push_str(&format!("    ({path:?}, {offset}, {len}),\n"));
    }
    generated.push_str("];\n");
    generated.push_str("pub(crate) static TRACKED_CONFIG_PATHS: &[&str] = &[\n");
    let mut standalone_paths = Vec::new();
    collect(&root, &root, &mut standalone_paths);
    standalone_paths.sort();
    let mut standalone_names = Vec::new();
    for file in standalone_paths {
        let relative = file
            .strip_prefix(&root)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        if relative.ends_with(".cfg.json") || relative.ends_with(".map.json") {
            standalone_names.push(relative.to_ascii_lowercase());
        }
    }
    standalone_names.sort();
    standalone_names.dedup();
    for path in standalone_names {
        generated.push_str(&format!("    {path:?},\n"));
    }
    generated.push_str("];\n");
    generated.push_str(&format!(
        "pub(crate) const TRACKED_CONFIG_FINGERPRINT: &str = \"{fingerprint:016x}\";\n"
    ));
    let out = PathBuf::from(std::env::var_os("OUT_DIR").expect("OUT_DIR"));
    fs::write(out.join("tracked_configs.rs"), generated).expect("write embedded configs");
    fs::write(out.join("gameplay_documents.bin"), payload).expect("write gameplay data");
    let plan: serde_json::Value = serde_json::from_slice(
        &fs::read(root.join("character_clips.json")).expect("read character clip plan"),
    )
    .expect("parse character clip plan");
    assert!(
        plan["schema_version"] == 1
            && plan["kind"] == "character_clips"
            && plan["characters"].is_object(),
        "invalid character clip plan"
    );
    fs::write(
        out.join("character_clip_plan.json"),
        serde_json::to_vec(&plan).expect("compact clip plan"),
    )
    .expect("write embedded clip plan");
}
