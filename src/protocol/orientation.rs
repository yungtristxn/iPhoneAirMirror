//! AirPlay configuration-header transform metadata. No pixel heuristics.
//!
//! The opcode is byte 4, not the combined u16 at 4..6. Byte 5 is the
//! sender's SymmetricTransform parameter. Independent protocol research and
//! local observations are recorded in docs/ROTATION_REGRESSION.md.
use anyhow::{Result, bail, ensure};

/// Explicit screen suspension from a complete AirPlay codec-config header.
/// Pinned UxPlay 15a6b87172eca38b0df4e615fbf1bba108734dde,
/// lib/raop_rtp_mirror.c:580-588, distinguishes these exact low flag bytes.
/// Unknown flags leave visibility unchanged; inactivity is not a lock signal.
pub fn configuration_suspended(header: &[u8]) -> Result<Option<bool>> {
    ensure!(
        header.len() == 128 && header[4] == 1,
        "screen-state configuration header"
    );
    Ok(match header[6] {
        0x56 | 0x5e => Some(true),
        0x16 | 0x1e => Some(false),
        _ => None,
    })
}

pub fn configuration_rotation(header: &[u8], rotation_advertised: bool) -> Result<u32> {
    ensure!(
        header.len() == 128 && header[4] == 1,
        "rotation configuration header"
    );
    // Legacy senders supply upright pixels when receiver-side rotation was not
    // advertised. Never reinterpret their high-byte metadata retroactively.
    if !rotation_advertised {
        return Ok(0);
    }
    // SymmetricTransform: bit 2 is a quarter-turn, bits 1 and 0 are the
    // two axis flips. Both flips compose to a half-turn. Reflection cases
    // are deliberately unsupported by the quarter-turn-only native path.
    match header[5] {
        0 => Ok(0),
        3 => Ok(180),
        4 => Ok(90),
        7 => Ok(270),
        value => bail!("unsupported AirPlay display transform {value}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_explicit_screen_flags_change_visibility() {
        let mut header = [0u8; 128];
        header[4] = 1;
        header[5] = 7; // Rotation is independent from screen suspension.
        header[7] = 1;
        for flags in 0..=255 {
            header[6] = flags;
            let expected = match flags {
                0x56 | 0x5e => Some(true),
                0x16 | 0x1e => Some(false),
                _ => None,
            };
            assert_eq!(configuration_suspended(&header).unwrap(), expected);
        }
        for len in 0..128 {
            assert!(configuration_suspended(&header[..len]).is_err());
        }
        assert!(configuration_suspended(&[0; 129]).is_err());
        for opcode in [0, 2, 5, 255] {
            header[4] = opcode;
            header[6] = 0x5e;
            assert!(configuration_suspended(&header).is_err());
        }
    }

    #[test]
    fn observed_transport_rotation_is_not_a_codec_sei_or_u16_opcode() {
        let mut header = [0u8; 128];
        header[4] = 1;
        header[6..8].copy_from_slice(&0x011eu16.to_le_bytes());
        for (transform, clockwise) in [(0, 0), (3, 180), (4, 90), (7, 270)] {
            header[5] = transform;
            assert_eq!(configuration_rotation(&header, true).unwrap(), clockwise);
            assert_eq!(configuration_rotation(&header, false).unwrap(), 0);
        }
        // Captured scalar regression: 0x0701 means opcode 1 + transform 7.
        assert_eq!(u16::from_le_bytes(header[4..6].try_into().unwrap()), 0x0701);
        header[5] = 0; // A later identity configuration replaces rotation.
        assert_eq!(configuration_rotation(&header, true).unwrap(), 0);
    }

    #[test]
    fn unknown_reflections_and_non_configuration_records_fail_closed() {
        let mut header = [0u8; 128];
        header[4] = 1;
        for value in 0..=255 {
            header[5] = value;
            assert_eq!(
                configuration_rotation(&header, true).is_ok(),
                matches!(value, 0 | 3 | 4 | 7)
            );
        }
        for end in 0..128 {
            assert!(configuration_rotation(&header[..end], true).is_err());
        }
        header[4] = 0; // A video packet's byte 5 may instead contain keyframe flags.
        header[5] = 0x10;
        assert!(configuration_rotation(&header, true).is_err());
        assert!(configuration_rotation(&[0; 129], true).is_err());
    }
}
