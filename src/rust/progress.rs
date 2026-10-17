//! Shared extraction progress: lock-free stage counters fed by workers, a fixed-cadence ticker
//! that forwards a monotone fraction to the caller, and an optional stderr renderer.

use super::CoreResult;
use std::collections::VecDeque;
use std::fmt::Write as _;
use std::io::{IsTerminal, Write};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

const STAGES: usize = 14;
const TICK: Duration = Duration::from_millis(80);
const WEIGHTS: [u32; STAGES] = [1, 2, 5, 2, 22, 7, 3, 6, 5, 2, 17, 26, 1, 1];
const READY: &str = "Ready — starting game…";

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Stage {
    Fingerprint,
    Extract,
    Textures,
    Audio,
    Models,
    Cleanup,
    Configs,
    Scripts,
    Packs,
    VerifyScripts,
    VerifyTrees,
    Seal,
    VerifyArchive,
    Publish,
}

impl Stage {
    pub const ALL: [Stage; STAGES] = [
        Stage::Fingerprint,
        Stage::Extract,
        Stage::Textures,
        Stage::Audio,
        Stage::Models,
        Stage::Cleanup,
        Stage::Configs,
        Stage::Scripts,
        Stage::Packs,
        Stage::VerifyScripts,
        Stage::VerifyTrees,
        Stage::Seal,
        Stage::VerifyArchive,
        Stage::Publish,
    ];

    /// Caller-facing status message.
    pub fn label(self) -> &'static str {
        match self {
            Stage::Fingerprint => "Checking source files…",
            Stage::Extract => "Extracting files…",
            Stage::Textures => "Converting textures…",
            Stage::Audio => "Converting audio…",
            Stage::Models => "Converting models and animation…",
            Stage::Cleanup => "Removing converted payloads…",
            Stage::Configs => "Converting configuration…",
            Stage::Scripts => "Converting scripts…",
            Stage::Packs => "Unpacking archives…",
            Stage::VerifyScripts => "Verifying scripts…",
            Stage::VerifyTrees => "Verifying converted data…",
            Stage::Seal => "Packing data…",
            Stage::VerifyArchive => "Verifying packed data…",
            Stage::Publish => "Publishing data…",
        }
    }

    /// Short console title.
    pub fn title(self) -> &'static str {
        match self {
            Stage::Fingerprint => "Fingerprint",
            Stage::Extract => "Extract",
            Stage::Textures => "Textures",
            Stage::Audio => "Audio",
            Stage::Models => "Models",
            Stage::Cleanup => "Cleanup",
            Stage::Configs => "Configs",
            Stage::Scripts => "Scripts",
            Stage::Packs => "Packs",
            Stage::VerifyScripts => "Verify scripts",
            Stage::VerifyTrees => "Verify trees",
            Stage::Seal => "Seal",
            Stage::VerifyArchive => "Verify archive",
            Stage::Publish => "Publish",
        }
    }

    /// Share of the overall bar; the table sums to 100.
    pub fn weight(self) -> u32 {
        WEIGHTS[self as usize]
    }

    fn bytes(self) -> bool {
        matches!(self, Stage::Fingerprint | Stage::Extract | Stage::Seal)
    }
}

#[derive(Default)]
struct StageState {
    done: AtomicU64,
    total: AtomicU64,
    bytes: AtomicU64,
    started: AtomicU64,
    finished: AtomicU64,
    skipped: AtomicBool,
    item: Mutex<String>,
}

struct Shared {
    origin: Instant,
    stages: [StageState; STAGES],
    logs: Mutex<Vec<String>>,
    footer: Mutex<Vec<String>>,
}

impl Shared {
    fn new() -> Self {
        Self {
            origin: Instant::now(),
            stages: Default::default(),
            logs: Mutex::default(),
            footer: Mutex::default(),
        }
    }

    fn now(&self) -> u64 {
        self.origin.elapsed().as_nanos() as u64 + 1
    }

    fn start(&self, stage: Stage) {
        let _ = self.stages[stage as usize].started.compare_exchange(
            0,
            self.now(),
            Ordering::Relaxed,
            Ordering::Relaxed,
        );
    }

    fn snapshot(&self) -> Snapshot {
        let stages = std::array::from_fn(|i| {
            let state = &self.stages[i];
            StageSnapshot {
                stage: Stage::ALL[i],
                done: state.done.load(Ordering::Relaxed),
                total: state.total.load(Ordering::Relaxed),
                bytes: state.bytes.load(Ordering::Relaxed),
                started: state.started.load(Ordering::Relaxed),
                finished: state.finished.load(Ordering::Relaxed),
                skipped: state.skipped.load(Ordering::Relaxed),
            }
        });
        Snapshot {
            elapsed: self.origin.elapsed(),
            stages,
        }
    }

    fn item(&self, stage: Stage) -> String {
        self.stages[stage as usize]
            .item
            .lock()
            .map(|item| item.clone())
            .unwrap_or_default()
    }
}

#[derive(Clone, Copy)]
struct StageSnapshot {
    stage: Stage,
    done: u64,
    total: u64,
    bytes: u64,
    started: u64,
    finished: u64,
    skipped: bool,
}

impl StageSnapshot {
    fn complete(&self) -> bool {
        self.skipped || self.finished != 0
    }

    fn active(&self) -> bool {
        self.started != 0 && !self.complete()
    }

    fn fraction(&self) -> f64 {
        if self.complete() {
            1.0
        } else if self.total == 0 {
            0.0
        } else {
            (self.done as f64 / self.total as f64).min(0.999)
        }
    }

    fn duration(&self, elapsed: Duration) -> Duration {
        if self.started == 0 {
            return Duration::ZERO;
        }
        let end = if self.finished != 0 {
            self.finished
        } else {
            elapsed.as_nanos() as u64 + 1
        };
        Duration::from_nanos(end.saturating_sub(self.started))
    }
}

struct Snapshot {
    elapsed: Duration,
    stages: [StageSnapshot; STAGES],
}

impl Snapshot {
    fn overall(&self) -> f64 {
        let sum: u32 = WEIGHTS.iter().sum();
        self.stages
            .iter()
            .map(|s| s.stage.weight() as f64 * s.fraction())
            .sum::<f64>()
            / sum as f64
    }

    fn lead(&self) -> Option<Stage> {
        self.stages
            .iter()
            .find(|s| s.active())
            .or_else(|| self.stages.iter().rev().find(|s| s.started != 0))
            .map(|s| s.stage)
    }
}

/// Cheap, cloneable handle workers report one stage's progress through.
#[derive(Clone)]
pub struct Phase {
    shared: Option<Arc<Shared>>,
    stage: Stage,
}

impl Phase {
    /// Detached handle whose reports go nowhere.
    pub fn noop() -> Phase {
        Phase {
            shared: None,
            stage: Stage::Fingerprint,
        }
    }

    pub fn stage(&self) -> Stage {
        self.stage
    }

    fn state(&self) -> Option<&StageState> {
        self.shared
            .as_ref()
            .map(|shared| &shared.stages[self.stage as usize])
    }

    /// Replaces the number of work units.
    pub fn set_total(&self, total: u64) {
        if let Some(state) = self.state() {
            state.total.store(total, Ordering::Relaxed);
        }
    }

    pub fn add_total(&self, n: u64) {
        if let Some(state) = self.state() {
            state.total.fetch_add(n, Ordering::Relaxed);
        }
    }

    /// Adds completed work units.
    pub fn inc(&self, n: u64) {
        if let Some(state) = self.state() {
            state.done.fetch_add(n, Ordering::Relaxed);
        }
    }

    /// Replaces the number of completed work units.
    pub fn set_done(&self, done: u64) {
        if let Some(state) = self.state() {
            state.done.store(done, Ordering::Relaxed);
        }
    }

    /// Adds to the bytes reported in the summary; not part of the fraction.
    pub fn add_bytes(&self, n: u64) {
        if let Some(state) = self.state() {
            state.bytes.fetch_add(n, Ordering::Relaxed);
        }
    }

    /// Names the unit currently in progress.
    pub fn item(&self, name: &str) {
        if let Some(mut item) = self.state().and_then(|state| state.item.lock().ok()) {
            item.clear();
            item.push_str(name);
        }
    }

    /// Prints an informational line above the live display.
    pub fn log(&self, line: impl Into<String>) {
        if let Some(mut logs) = self.shared.as_ref().and_then(|s| s.logs.lock().ok()) {
            logs.push(line.into());
        }
    }

    /// Marks the stage finished regardless of its counters.
    pub fn done(&self) {
        if let Some(shared) = &self.shared {
            shared.start(self.stage);
            let _ = shared.stages[self.stage as usize]
                .finished
                .compare_exchange(0, shared.now(), Ordering::Relaxed, Ordering::Relaxed);
        }
    }

    /// Clears the counters of a stage that will run again.
    pub fn reset(&self) {
        if let Some(state) = self.state() {
            state.done.store(0, Ordering::Relaxed);
            state.total.store(0, Ordering::Relaxed);
            state.bytes.store(0, Ordering::Relaxed);
            state.finished.store(0, Ordering::Relaxed);
            state.started.store(0, Ordering::Relaxed);
            self.item("");
        }
    }
}

/// Forwards a monotone fraction to the caller's sink.
struct Emitter {
    best: f32,
    message: &'static str,
}

/// Owns one extraction's progress state, its ticker thread and its console output.
pub struct Reporter<'a> {
    shared: Arc<Shared>,
    sink: &'a (dyn Fn(f32, &'static str) + Sync),
    emitter: Mutex<Emitter>,
    console: bool,
}

impl<'a> Reporter<'a> {
    pub fn new(sink: &'a (dyn Fn(f32, &'static str) + Sync)) -> Self {
        Self {
            shared: Arc::new(Shared::new()),
            sink,
            emitter: Mutex::new(Emitter {
                best: -1.0,
                message: "",
            }),
            console: false,
        }
    }

    /// Renders progress on stderr when it is attached to something.
    pub fn with_console(mut self) -> Self {
        self.console = true;
        self
    }

    /// Runs `body` while a ticker publishes progress, then prints the summary.
    pub fn run<T>(self, body: impl FnOnce(&Reporter<'a>) -> CoreResult<T>) -> CoreResult<T> {
        let console = if self.console {
            Console::detect()
        } else {
            None
        };
        let signal = (Mutex::new(false), Condvar::new());
        let (result, console) = std::thread::scope(|scope| {
            let ticker = std::thread::Builder::new()
                .name("dc2data-progress".into())
                .spawn_scoped(scope, || self.tick(&signal, console))
                .expect("progress ticker");
            let stop = StopOnDrop(&signal);
            let result = body(&self);
            drop(stop);
            (result, ticker.join().ok().flatten())
        });
        let snapshot = self.shared.snapshot();
        let succeeded = result.is_ok() && snapshot.stages[Stage::Publish as usize].complete();
        if succeeded {
            self.emit_final();
        }
        if let Some(mut console) = console {
            console.flush_logs(&self.shared);
            console.summary(&self.shared, &snapshot, succeeded);
        }
        result
    }

    /// Starts a stage and returns its handle.
    pub fn phase(&self, stage: Stage) -> Phase {
        self.shared.start(stage);
        self.emit();
        Phase {
            shared: Some(self.shared.clone()),
            stage,
        }
    }

    /// Marks a stage finished.
    pub fn done(&self, stage: Stage) {
        self.phase(stage).done();
        self.emit();
    }

    /// Marks a stage this run does not execute as finished.
    pub fn skip(&self, stage: Stage) {
        self.shared.stages[stage as usize]
            .skipped
            .store(true, Ordering::Relaxed);
        self.emit();
    }

    /// Prints an informational line above the live display.
    pub fn log(&self, line: impl Into<String>) {
        if let Ok(mut logs) = self.shared.logs.lock() {
            logs.push(line.into());
        }
    }

    /// Adds a closing line to the summary.
    pub fn footer(&self, line: impl Into<String>) {
        if let Ok(mut footer) = self.shared.footer.lock() {
            footer.push(line.into());
        }
    }

    pub fn overall(&self) -> f32 {
        self.shared.snapshot().overall() as f32
    }

    fn emit(&self) {
        let snapshot = self.shared.snapshot();
        let fraction = (snapshot.overall() as f32).min(0.995);
        let message = snapshot
            .lead()
            .map_or(Stage::Fingerprint.label(), Stage::label);
        let Ok(mut emitter) = self.emitter.lock() else {
            return;
        };
        if fraction < emitter.best {
            if message == emitter.message {
                return;
            }
        } else if fraction - emitter.best < 0.001 && message == emitter.message {
            return;
        }
        emitter.best = emitter.best.max(fraction);
        emitter.message = message;
        (self.sink)(emitter.best, message);
    }

    fn emit_final(&self) {
        if let Ok(mut emitter) = self.emitter.lock() {
            emitter.best = 1.0;
            emitter.message = READY;
            (self.sink)(1.0, READY);
        }
    }

    fn tick(
        &self,
        signal: &(Mutex<bool>, Condvar),
        mut console: Option<Console>,
    ) -> Option<Console> {
        let (lock, condvar) = signal;
        let mut stopped = lock.lock().ok()?;
        loop {
            self.emit();
            if let Some(console) = &mut console {
                console.frame(&self.shared);
            }
            if *stopped {
                return console;
            }
            stopped = condvar.wait_timeout(stopped, TICK).ok()?.0;
            if *stopped {
                return console;
            }
        }
    }
}

struct StopOnDrop<'s>(&'s (Mutex<bool>, Condvar));

impl Drop for StopOnDrop<'_> {
    fn drop(&mut self) {
        if let Ok(mut stopped) = self.0.0.lock() {
            *stopped = true;
        }
        self.0.1.notify_all();
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    Plain,
    Tty { unicode: bool, color: bool },
}

struct Glyphs {
    spinner: &'static [&'static str],
    full: &'static str,
    empty: &'static str,
    active: &'static str,
    check: &'static str,
    ellipsis: &'static str,
    dot: &'static str,
}

const UNICODE: Glyphs = Glyphs {
    spinner: &["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"],
    full: "█",
    empty: "░",
    active: "▸",
    check: "✓",
    ellipsis: "…",
    dot: "·",
};

const ASCII: Glyphs = Glyphs {
    spinner: &["|", "/", "-", "\\"],
    full: "#",
    empty: ".",
    active: ">",
    check: "+",
    ellipsis: "~",
    dot: "-",
};

/// Rate of overall progress over a sliding window, for the time-left estimate.
#[derive(Default)]
struct Rate {
    samples: VecDeque<(f64, f64)>,
}

impl Rate {
    fn eta(&mut self, elapsed: f64, fraction: f64) -> Option<f64> {
        self.samples.push_back((elapsed, fraction));
        while self
            .samples
            .front()
            .is_some_and(|&(t, _)| elapsed - t > 15.0)
        {
            self.samples.pop_front();
        }
        let &(t0, f0) = self.samples.front()?;
        if fraction < 0.03 || elapsed < 5.0 {
            return None;
        }
        let average = fraction / elapsed;
        let window = if elapsed - t0 >= 2.0 {
            ((fraction - f0) / (elapsed - t0)).max(0.0)
        } else {
            average
        };
        let rate = 0.7 * window + 0.3 * average;
        Some((1.0 - fraction) / rate)
    }
}

/// stderr renderer: a redrawn block on terminals, throttled lines otherwise.
struct Console {
    mode: Mode,
    out: std::io::Stderr,
    lines: usize,
    frame: usize,
    rate: Rate,
    eta: Option<f64>,
    printed: Option<(Instant, f64, Vec<Stage>)>,
    best: f64,
}

impl Console {
    fn detect() -> Option<Console> {
        if cfg!(test) {
            return None;
        }
        let forced = std::env::var("DC2DATA_PROGRESS").unwrap_or_default();
        let tty = match forced.to_ascii_lowercase().as_str() {
            "off" | "none" | "0" => return None,
            "plain" => false,
            "tty" => {
                terminal::enable_vt();
                true
            }
            _ => std::io::stderr().is_terminal() && terminal::enable_vt(),
        };
        let mode = if tty {
            Mode::Tty {
                unicode: terminal::unicode(),
                color: std::env::var_os("NO_COLOR").is_none_or(|v| v.is_empty()),
            }
        } else {
            Mode::Plain
        };
        if tty {
            terminal::restore_cursor_on_interrupt();
        }
        let mut console = Console {
            mode,
            out: std::io::stderr(),
            lines: 0,
            frame: 0,
            rate: Rate::default(),
            eta: None,
            printed: None,
            best: 0.0,
        };
        if tty {
            console.write("\x1b[?25l");
        }
        Some(console)
    }

    fn write(&mut self, text: &str) {
        let mut out = self.out.lock();
        let _ = out.write_all(text.as_bytes());
        let _ = out.flush();
    }

    fn glyphs(&self) -> &'static Glyphs {
        match self.mode {
            Mode::Tty { unicode: true, .. } => &UNICODE,
            _ => &ASCII,
        }
    }

    fn paint(&self, code: &str, text: &str) -> String {
        match self.mode {
            Mode::Tty { color: true, .. } if !text.is_empty() => {
                format!("\x1b[{code}m{text}\x1b[0m")
            }
            _ => text.to_owned(),
        }
    }

    fn clear(&self) -> String {
        match self.lines {
            0 => String::new(),
            1 => "\r\x1b[J".into(),
            n => format!("\r\x1b[{}A\x1b[J", n - 1),
        }
    }

    fn take_logs(shared: &Shared) -> Vec<String> {
        shared
            .logs
            .lock()
            .map(|mut logs| std::mem::take(&mut *logs))
            .unwrap_or_default()
    }

    fn frame(&mut self, shared: &Shared) {
        let snapshot = shared.snapshot();
        let overall = snapshot.overall().min(0.999);
        self.best = self.best.max(overall);
        let eta = self.rate.eta(snapshot.elapsed.as_secs_f64(), self.best);
        self.eta = match (self.eta, eta) {
            (Some(previous), Some(next)) => {
                Some(0.85 * (previous - TICK.as_secs_f64()) + 0.15 * next)
            }
            (_, next) => next,
        };
        self.frame += 1;
        let logs = Self::take_logs(shared);
        match self.mode {
            Mode::Plain => self.plain(&snapshot, logs),
            Mode::Tty { .. } => self.tty(shared, &snapshot, logs),
        }
    }

    fn flush_logs(&mut self, shared: &Shared) {
        let logs = Self::take_logs(shared);
        match self.mode {
            Mode::Plain => self.plain_logs(logs),
            Mode::Tty { .. } => {
                let mut text = self.clear();
                self.lines = 0;
                for line in logs {
                    text.push_str(&line);
                    text.push('\n');
                }
                self.write(&text);
            }
        }
    }

    fn plain_logs(&mut self, logs: Vec<String>) {
        if logs.is_empty() {
            return;
        }
        let mut text = String::new();
        for line in logs {
            let _ = writeln!(text, "dc2data: {line}");
        }
        self.write(&text);
    }

    fn plain(&mut self, snapshot: &Snapshot, logs: Vec<String>) {
        self.plain_logs(logs);
        let active: Vec<Stage> = snapshot
            .stages
            .iter()
            .filter(|s| s.active())
            .map(|s| s.stage)
            .collect();
        let now = Instant::now();
        let due = match &self.printed {
            None => true,
            Some((at, fraction, stages)) => {
                *stages != active
                    || now - *at >= Duration::from_secs(10)
                    || (self.best - fraction >= 0.05 && now - *at >= Duration::from_secs(2))
            }
        };
        if !due || active.is_empty() {
            return;
        }
        let parts: Vec<String> = snapshot
            .stages
            .iter()
            .filter(|s| s.active())
            .map(|s| {
                if s.total == 0 {
                    s.stage.title().to_owned()
                } else {
                    format!(
                        "{} {:.0}% ({})",
                        s.stage.title(),
                        s.fraction() * 100.0,
                        counts(s)
                    )
                }
            })
            .collect();
        let mut line = format!(
            "dc2data: [{:>3.0}%] {} - {} elapsed",
            self.best * 100.0,
            parts.join(", "),
            clock(snapshot.elapsed)
        );
        if let Some(eta) = self.eta {
            let _ = write!(
                line,
                ", ~{} left",
                clock(Duration::from_secs_f64(eta.max(0.0)))
            );
        }
        line.push('\n');
        self.write(&line);
        self.printed = Some((now, self.best, active));
    }

    fn tty(&mut self, shared: &Shared, snapshot: &Snapshot, logs: Vec<String>) {
        let width = terminal::width();
        let glyphs = self.glyphs();
        let mut lines = Vec::new();
        let spinner = glyphs.spinner[self.frame % glyphs.spinner.len()];
        let mut tail = format!("{} elapsed", clock(snapshot.elapsed));
        if let Some(eta) = self.eta {
            let _ = write!(
                tail,
                " {} ~{} left",
                glyphs.dot,
                clock(Duration::from_secs_f64(eta.max(0.0)))
            );
        }
        let percent = format!("{:>3.0}%", self.best * 100.0);
        let fixed = 1 + 1 + 1 + 7 + 2 + 2 + percent.len() + 3 + tail.chars().count();
        let bar_width = width.saturating_sub(fixed).clamp(10, 48);
        lines.push(format!(
            " {} {}  {}  {}   {}",
            self.paint("1;36", spinner),
            self.paint("1", "dc2data"),
            self.bar(self.best, bar_width, "36"),
            self.paint("1", &percent),
            self.paint("2", &tail)
        ));
        let rows: Vec<&StageSnapshot> = snapshot.stages.iter().filter(|s| s.active()).collect();
        for s in rows.iter().take(4) {
            let title = format!("{:<14}", s.stage.title());
            let count = if s.total == 0 {
                String::new()
            } else {
                counts(s)
            };
            let mut row = format!(
                "   {} {} {:>15}  ",
                self.paint("1;36", glyphs.active),
                self.paint("1", &title),
                count
            );
            let mut used = 3 + 2 + 14 + 1 + 15 + 2;
            if s.total == 0 {
                let pulse = self.pulse(14);
                row.push_str(&pulse);
                row.push_str("      ");
            } else {
                row.push_str(&self.bar(s.fraction(), 14, "36"));
                let _ = write!(row, " {:>4.0}%", s.fraction() * 100.0);
            }
            used += 14 + 6;
            let item = shared.item(s.stage);
            let room = width.saturating_sub(used + 3);
            if !item.is_empty() && room > 8 {
                row.push_str("  ");
                row.push_str(&self.paint("2", &truncate(&item, room, glyphs.ellipsis)));
            }
            lines.push(row);
        }
        if rows.len() > 4 {
            lines.push(format!("     {} and {} more", glyphs.dot, rows.len() - 4));
        }
        let mut text = self.clear();
        for line in logs {
            text.push_str(&line);
            text.push('\n');
        }
        text.push_str(&lines.join("\n"));
        self.lines = lines.len();
        self.write(&text);
    }

    fn bar(&self, fraction: f64, width: usize, color: &str) -> String {
        let glyphs = self.glyphs();
        let filled = ((fraction.clamp(0.0, 1.0) * width as f64).round() as usize).min(width);
        format!(
            "{}{}",
            self.paint(color, &glyphs.full.repeat(filled)),
            self.paint("2", &glyphs.empty.repeat(width - filled))
        )
    }

    fn pulse(&self, width: usize) -> String {
        let glyphs = self.glyphs();
        let span = 4.min(width);
        let travel = 2 * (width - span);
        let step = self.frame % travel.max(1);
        let at = if step > width - span {
            travel - step
        } else {
            step
        };
        format!(
            "{}{}{}",
            self.paint("2", &glyphs.empty.repeat(at)),
            self.paint("36", &glyphs.full.repeat(span)),
            self.paint("2", &glyphs.empty.repeat(width - span - at))
        )
    }

    fn summary(&mut self, shared: &Shared, snapshot: &Snapshot, succeeded: bool) {
        let glyphs = self.glyphs();
        let mut text = String::new();
        if succeeded {
            let _ = writeln!(
                text,
                "dc2data: {} done in {}",
                self.paint("1;32", glyphs.check),
                clock(snapshot.elapsed)
            );
        } else {
            let at = snapshot
                .stages
                .iter()
                .find(|s| s.active())
                .map_or("", |s| s.stage.title());
            let _ = writeln!(
                text,
                "dc2data: {} stopped after {}{}{}",
                self.paint("1;31", "x"),
                clock(snapshot.elapsed),
                if at.is_empty() { "" } else { " during " },
                at
            );
        }
        let _ = writeln!(
            text,
            "{}",
            self.paint(
                "2",
                &format!(
                    "  {:<14} {:>7} {:>7} {:>15} {:>10}",
                    "stage", "start", "time", "work", "bytes"
                )
            )
        );
        for s in snapshot
            .stages
            .iter()
            .filter(|s| s.started != 0 && !s.skipped)
        {
            let mark = if s.complete() {
                self.paint("32", glyphs.check)
            } else {
                self.paint("31", "x")
            };
            let work = if s.total == 0 && s.done == 0 {
                "-".to_owned()
            } else if s.stage.bytes() {
                size(s.done.max(s.total))
            } else {
                group(s.done.max(if s.complete() { s.total } else { 0 }))
            };
            let bytes = if s.bytes == 0 {
                "-".into()
            } else {
                size(s.bytes)
            };
            let _ = writeln!(
                text,
                "{} {:<14} {:>7} {:>7} {:>15} {:>10}",
                mark,
                s.stage.title(),
                format!("+{}", clock(Duration::from_nanos(s.started - 1))),
                clock(s.duration(snapshot.elapsed)),
                work,
                bytes
            );
        }
        if let Ok(footer) = shared.footer.lock() {
            for line in footer.iter() {
                let _ = writeln!(text, "  {line}");
            }
        }
        if let Mode::Plain = self.mode {
            self.write(&text);
            return;
        }
        let mut out = self.clear();
        self.lines = 0;
        out.push_str(&text);
        self.write(&out);
    }
}

impl Drop for Console {
    fn drop(&mut self) {
        if let Mode::Tty { .. } = self.mode {
            let mut text = self.clear();
            self.lines = 0;
            text.push_str("\x1b[?25h");
            self.write(&text);
        }
    }
}

fn counts(s: &StageSnapshot) -> String {
    if s.stage.bytes() {
        format!("{}/{}", size(s.done), size(s.total))
    } else {
        format!("{}/{}", group(s.done), group(s.total))
    }
}

fn clock(duration: Duration) -> String {
    let seconds = duration.as_secs();
    if seconds >= 3600 {
        format!(
            "{}:{:02}:{:02}",
            seconds / 3600,
            seconds / 60 % 60,
            seconds % 60
        )
    } else {
        format!("{:02}:{:02}", seconds / 60, seconds % 60)
    }
}

pub(crate) fn group(value: u64) -> String {
    let digits = value.to_string();
    let mut out = String::with_capacity(digits.len() + digits.len() / 3);
    for (i, c) in digits.chars().enumerate() {
        if i > 0 && (digits.len() - i).is_multiple_of(3) {
            out.push(',');
        }
        out.push(c);
    }
    out
}

pub(crate) fn size(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    let mut value = bytes as f64;
    let mut unit = 0;
    while value >= 1000.0 && unit + 1 < UNITS.len() {
        value /= 1000.0;
        unit += 1;
    }
    if unit == 0 {
        format!("{bytes} B")
    } else {
        format!("{value:.1} {}", UNITS[unit])
    }
}

fn truncate(text: &str, width: usize, ellipsis: &str) -> String {
    let count = text.chars().count();
    if count <= width {
        return text.to_owned();
    }
    let keep = width.saturating_sub(1);
    let tail: String = text.chars().skip(count - keep).collect();
    format!("{ellipsis}{tail}")
}

#[cfg(windows)]
mod terminal {
    use std::ffi::c_void;

    const STD_ERROR_HANDLE: u32 = 0xFFFF_FFF4;
    const ENABLE_VIRTUAL_TERMINAL_PROCESSING: u32 = 0x0004;

    #[repr(C)]
    #[derive(Default)]
    struct ScreenBufferInfo {
        size: [i16; 2],
        cursor: [i16; 2],
        attributes: u16,
        window: [i16; 4],
        maximum: [i16; 2],
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetStdHandle(handle: u32) -> *mut c_void;
        fn GetConsoleMode(handle: *mut c_void, mode: *mut u32) -> i32;
        fn SetConsoleMode(handle: *mut c_void, mode: u32) -> i32;
        fn GetConsoleScreenBufferInfo(handle: *mut c_void, info: *mut ScreenBufferInfo) -> i32;
        fn SetConsoleCtrlHandler(
            handler: Option<unsafe extern "system" fn(u32) -> i32>,
            add: i32,
        ) -> i32;
    }

    /// Enables escape sequences; terminals that are not consoles (mintty) already support them.
    pub fn enable_vt() -> bool {
        unsafe {
            let handle = GetStdHandle(STD_ERROR_HANDLE);
            let mut mode = 0;
            if GetConsoleMode(handle, &mut mode) == 0 {
                return true;
            }
            mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING != 0
                || SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0
        }
    }

    pub fn unicode() -> bool {
        true
    }

    pub fn width() -> usize {
        let mut info = ScreenBufferInfo::default();
        let columns = if unsafe {
            GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &mut info)
        } != 0
        {
            (info.window[2] - info.window[0] + 1) as usize
        } else {
            super::columns()
        };
        columns.clamp(60, 160)
    }

    unsafe extern "system" fn interrupted(_: u32) -> i32 {
        use std::io::Write;
        let _ = std::io::stderr().write_all(b"\x1b[?25h\n");
        0
    }

    pub fn restore_cursor_on_interrupt() {
        unsafe {
            SetConsoleCtrlHandler(Some(interrupted), 1);
        }
    }
}

#[cfg(unix)]
mod terminal {
    unsafe extern "C" {
        fn signal(signum: i32, handler: usize) -> usize;
        fn raise(signum: i32) -> i32;
        fn write(fd: i32, buf: *const u8, count: usize) -> isize;
    }

    pub fn enable_vt() -> bool {
        std::env::var("TERM").is_ok_and(|term| term != "dumb")
    }

    pub fn unicode() -> bool {
        ["LC_ALL", "LC_CTYPE", "LANG"]
            .iter()
            .filter_map(|name| std::env::var(name).ok())
            .find(|value| !value.is_empty())
            .is_some_and(|value| value.to_ascii_uppercase().replace('-', "").contains("UTF8"))
    }

    pub fn width() -> usize {
        super::columns().clamp(60, 160)
    }

    extern "C" fn interrupted(signum: i32) {
        let show = b"\x1b[?25h\n";
        unsafe {
            write(2, show.as_ptr(), show.len());
            signal(signum, 0);
            raise(signum);
        }
    }

    pub fn restore_cursor_on_interrupt() {
        unsafe {
            signal(2, interrupted as extern "C" fn(i32) as usize);
            signal(15, interrupted as extern "C" fn(i32) as usize);
        }
    }
}

#[cfg(not(any(windows, unix)))]
mod terminal {
    pub fn enable_vt() -> bool {
        false
    }
    pub fn unicode() -> bool {
        false
    }
    pub fn width() -> usize {
        super::columns().clamp(60, 160)
    }
    pub fn restore_cursor_on_interrupt() {}
}

fn columns() -> usize {
    std::env::var("COLUMNS")
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(80)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn weights_sum_to_one_hundred() {
        assert_eq!(WEIGHTS.iter().sum::<u32>(), 100);
    }

    #[test]
    fn sink_is_monotone_and_finishes_only_after_publication() {
        let seen = Mutex::new(Vec::new());
        let sink =
            |fraction: f32, message: &'static str| seen.lock().unwrap().push((fraction, message));
        Reporter::new(&sink)
            .run(|reporter| {
                let extract = reporter.phase(Stage::Extract);
                extract.set_total(4);
                extract.inc(2);
                reporter.phase(Stage::Textures);
                extract.inc(2);
                reporter.done(Stage::Extract);
                for stage in Stage::ALL {
                    reporter.done(stage);
                }
                Ok(())
            })
            .unwrap();
        let seen = seen.into_inner().unwrap();
        assert!(seen.windows(2).all(|pair| pair[0].0 <= pair[1].0));
        assert_eq!(seen.last(), Some(&(1.0, READY)));
        assert_eq!(
            seen.iter().filter(|(fraction, _)| *fraction >= 1.0).count(),
            1
        );
    }

    #[test]
    fn failed_run_never_reports_completion() {
        let seen = Mutex::new(Vec::new());
        let sink = |fraction: f32, _: &'static str| seen.lock().unwrap().push(fraction);
        let result: CoreResult<()> = Reporter::new(&sink).run(|reporter| {
            reporter.phase(Stage::Extract).inc(1);
            Err(super::super::CoreError("failed".into()))
        });
        assert!(result.is_err());
        assert!(seen.into_inner().unwrap().iter().all(|f| *f < 1.0));
    }

    #[test]
    fn formatting() {
        assert_eq!(group(1234567), "1,234,567");
        assert_eq!(group(12), "12");
        assert_eq!(size(4_300_000_000), "4.3 GB");
        assert_eq!(clock(Duration::from_secs(401)), "06:41");
        assert_eq!(truncate("chr/ally/max.chr", 8, "…"), "…max.chr");
    }
}
