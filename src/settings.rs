//! Read-only, bounded local display geometry. This is an advisory request, not a
//! crop, an orientation override, or automatically learned sender metadata.
use anyhow::{Context, Result, ensure};
use std::{fs::File, io::Read, path::Path};

const MAX_ASPECT_BYTES: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DisplayAspect {
    width: u32,
    height: u32,
}
impl DisplayAspect {
    fn parse(bytes: &[u8]) -> Result<Self> {
        ensure!(
            bytes.len() <= MAX_ASPECT_BYTES,
            "display-aspect.txt exceeds 64 bytes"
        );
        let text =
            std::str::from_utf8(bytes).context("display-aspect.txt must contain UTF-8 text")?;
        let mut fields = text.split_ascii_whitespace();
        let mut dimension = || -> Result<u32> {
            let field = fields
                .next()
                .context("display-aspect.txt requires width and height")?;
            ensure!(
                field.bytes().all(|byte| byte.is_ascii_digit()),
                "display aspect dimensions must be positive integers"
            );
            let value = field
                .parse::<u32>()
                .context("display aspect dimension exceeds u32")?;
            ensure!(
                (48..=3840).contains(&value),
                "display aspect dimensions must be from 48 to 3840 pixels"
            );
            Ok(value)
        };
        let width = dimension()?;
        let height = dimension()?;
        ensure!(
            fields.next().is_none(),
            "display-aspect.txt must contain exactly width and height"
        );
        Ok(Self { width, height })
    }
    pub fn width_for(self, target_height: u32) -> Result<u32> {
        ensure!(target_height > 0, "target display height must be positive");
        let numerator = u64::from(self.width)
            .checked_mul(u64::from(target_height))
            .context("display aspect scaling overflow")?;
        let divisor = u64::from(self.height)
            .checked_mul(2)
            .context("display aspect divisor overflow")?;
        // Round to the nearest even pixel width, rounding exact ties upward.
        let pairs = numerator
            .checked_add(u64::from(self.height))
            .context("display aspect rounding overflow")?
            / divisor;
        let width = pairs
            .checked_mul(2)
            .context("display aspect width overflow")?;
        ensure!(
            width > 0,
            "scaled display width is below one even pixel pair"
        );
        u32::try_from(width).context("scaled display width exceeds u32")
    }
}
fn read_display_aspect(reader: impl Read) -> Result<DisplayAspect> {
    // The extra byte detects a file growing after its metadata was checked.
    let mut bytes = Vec::with_capacity(MAX_ASPECT_BYTES + 1);
    reader
        .take((MAX_ASPECT_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
        .context("could not read display-aspect.txt")?;
    DisplayAspect::parse(&bytes)
}
pub fn load_display_aspect(path: &Path) -> Result<Option<DisplayAspect>> {
    let file = match File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error).context("could not open display-aspect.txt"),
    };
    ensure!(
        file.metadata()?.len() <= MAX_ASPECT_BYTES as u64,
        "display-aspect.txt exceeds 64 bytes"
    );
    Ok(Some(read_display_aspect(file)?))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn aspect_requires_exactly_two_bounded_integer_dimensions() {
        let expected = DisplayAspect {
            width: 994,
            height: 2160,
        };
        assert_eq!(DisplayAspect::parse(b"994 2160\r\n").unwrap(), expected);
        assert_eq!(DisplayAspect::parse(b"\t994\n2160 ").unwrap(), expected);
        for invalid in [
            &b""[..],
            &b"994"[..],
            &b"994 2160 1"[..],
            &b"994.0 2160"[..],
            &b"+994 2160"[..],
            &b"-994 2160"[..],
            &b"0 2160"[..],
            &b"47 2160"[..],
            &b"994 3841"[..],
            &b"4294967296 2160"[..],
            &b"994\0 2160"[..],
            &[255, 32, 49][..],
        ] {
            assert!(DisplayAspect::parse(invalid).is_err());
        }
        assert!(DisplayAspect::parse(b"48 3840").is_ok());
    }
    #[test]
    fn aspect_scaling_rounds_to_nearest_even_without_changing_height() {
        let aspect = DisplayAspect::parse(b"994 2160").unwrap();
        assert_eq!(aspect.width_for(2160).unwrap(), 994);
        assert_eq!(aspect.width_for(1440).unwrap(), 662);
        assert_eq!(aspect.width_for(1080).unwrap(), 498);
        assert!(aspect.width_for(0).is_err());
        let wide = DisplayAspect::parse(b"3840 48").unwrap();
        assert!(wide.width_for(u32::MAX).is_err());
        assert_eq!(
            DisplayAspect::parse(b"1920 1080")
                .unwrap()
                .width_for(2160)
                .unwrap(),
            3840
        );
    }
    #[test]
    fn growing_or_unbounded_input_stops_after_one_limit_sentinel() {
        struct Endless(usize);
        impl Read for Endless {
            fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
                buffer.fill(b'0');
                self.0 += buffer.len();
                Ok(buffer.len())
            }
        }
        let mut input = Endless(0);
        assert!(read_display_aspect(&mut input).is_err());
        assert_eq!(input.0, 65);
    }
    #[test]
    fn settings_file_is_optional_bounded_and_never_modified() {
        struct Temporary(std::path::PathBuf);
        impl Drop for Temporary {
            fn drop(&mut self) {
                let _ = std::fs::remove_file(&self.0);
            }
        }
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = Temporary(std::env::temp_dir().join(format!(
            "airmirror-display-aspect-{}-{nonce}.txt",
            std::process::id()
        )));
        assert_eq!(load_display_aspect(&path.0).unwrap(), None);
        let mut valid = b"994 2160".to_vec();
        valid.resize(64, b' ');
        std::fs::write(&path.0, &valid).unwrap();
        assert_eq!(
            load_display_aspect(&path.0)
                .unwrap()
                .unwrap()
                .width_for(2160)
                .unwrap(),
            994
        );
        assert_eq!(std::fs::read(&path.0).unwrap(), valid);
        valid.push(b' ');
        std::fs::write(&path.0, &valid).unwrap();
        assert!(load_display_aspect(&path.0).is_err());
        assert_eq!(std::fs::read(&path.0).unwrap(), valid);
        std::fs::write(&path.0, b"invalid").unwrap();
        assert!(load_display_aspect(&path.0).is_err());
        assert_eq!(std::fs::read(&path.0).unwrap(), b"invalid");
    }
}
