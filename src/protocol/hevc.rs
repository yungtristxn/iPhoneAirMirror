//! Bounded AirPlay hvc1/hvcC configuration and HEVC access-unit validation.
//! Only Main-profile, 8-bit 4:2:0, single-layer SDR is admitted to Windows.
use anyhow::{Result, ensure};
pub const MAX_FRAME: usize = 4 * 1024 * 1024;
pub const MAX_WIDTH: u32 = 3840;
pub const MAX_HEIGHT: u32 = 2160;

fn kind(nal: &[u8]) -> Result<u8> {
    ensure!(nal.len() >= 2 && nal[0] & 0x80 == 0, "HEVC NAL header");
    ensure!(
        nal[0] & 1 == 0 && nal[1] >> 3 == 0 && nal[1] & 7 != 0,
        "HEVC layer/temporal ID"
    );
    ensure!(
        !nal[2..].windows(3).any(|w| w == [0, 0, 1]),
        "embedded HEVC start code"
    );
    Ok((nal[0] >> 1) & 63)
}
pub fn dimensions(nal: &[u8]) -> Result<(u32, u32)> {
    let info = sequence_info(nal)?;
    Ok((info.width, info.height))
}
struct SequenceInfo {
    width: u32,
    height: u32,
    max_num_reorder_pics: u32,
}
fn sequence_info(nal: &[u8]) -> Result<SequenceInfo> {
    ensure!(nal.len() <= 16384 && kind(nal)? == 33, "HEVC SPS size/type");
    let mut rbsp = Vec::with_capacity(nal.len());
    let mut zeros = 0;
    for (index, &v) in nal[2..].iter().enumerate() {
        if zeros >= 2 && v == 3 {
            ensure!(
                nal.get(index + 3).is_some_and(|&next| next <= 3),
                "invalid HEVC emulation prevention"
            );
            zeros = 0;
            continue;
        }
        rbsp.push(v);
        zeros = if v == 0 { zeros + 1 } else { 0 };
    }
    struct Bits<'a> {
        bytes: &'a [u8],
        bit: usize,
    }
    impl Bits<'_> {
        fn n(&mut self, n: usize) -> Result<u32> {
            ensure!(
                n <= 32 && self.bit + n <= self.bytes.len() * 8,
                "truncated HEVC SPS"
            );
            let mut v = 0;
            for _ in 0..n {
                v = (v << 1) | u32::from((self.bytes[self.bit / 8] >> (7 - self.bit % 8)) & 1);
                self.bit += 1;
            }
            Ok(v)
        }
        fn ue(&mut self) -> Result<u32> {
            let mut zeros = 0;
            while self.n(1)? == 0 {
                zeros += 1;
                ensure!(zeros <= 24, "HEVC Golomb budget");
            }
            Ok((1 << zeros) - 1 + self.n(zeros)?)
        }
    }
    let mut b = Bits {
        bytes: &rbsp,
        bit: 0,
    };
    b.n(4)?;
    let layers = b.n(3)? as usize;
    ensure!(layers <= 6, "HEVC sublayer budget");
    b.n(1)?;
    ensure!(b.n(2)? == 0, "HEVC profile space");
    b.n(1)?;
    ensure!(b.n(5)? == 1, "only HEVC Main profile");
    b.n(32)?;
    b.n(32)?;
    b.n(16)?;
    ensure!(b.n(8)? <= 153, "HEVC level budget");
    let mut profile = [false; 7];
    let mut level = [false; 7];
    for i in 0..layers {
        profile[i] = b.n(1)? != 0;
        level[i] = b.n(1)? != 0;
    }
    if layers > 0 {
        for _ in layers..8 {
            ensure!(b.n(2)? == 0, "HEVC reserved sublayer bits");
        }
    }
    for i in 0..layers {
        if profile[i] {
            b.n(32)?;
            b.n(32)?;
            b.n(24)?;
        }
        if level[i] {
            b.n(8)?;
        }
    }
    ensure!(b.ue()? <= 15, "HEVC SPS ID");
    ensure!(b.ue()? == 1, "only HEVC 4:2:0");
    let w = b.ue()?;
    let h = b.ue()?;
    ensure!(
        (48..=MAX_WIDTH).contains(&w) && (48..=MAX_HEIGHT).contains(&h),
        "HEVC coded dimensions"
    );
    let (mut left, mut right, mut top, mut bottom) = (0, 0, 0, 0);
    if b.n(1)? != 0 {
        left = b.ue()?;
        right = b.ue()?;
        top = b.ue()?;
        bottom = b.ue()?;
    }
    let width = w
        .checked_sub((left + right) * 2)
        .ok_or_else(|| anyhow::anyhow!("HEVC horizontal crop"))?;
    let height = h
        .checked_sub((top + bottom) * 2)
        .ok_or_else(|| anyhow::anyhow!("HEVC vertical crop"))?;
    ensure!(width > 0 && height > 0, "HEVC display dimensions");
    ensure!(b.ue()? == 0 && b.ue()? == 0, "only 8-bit HEVC SDR");
    ensure!(b.ue()? <= 12, "HEVC POC budget");
    let all = b.n(1)? != 0;
    let mut max_num_reorder_pics = 0;
    for _ in (if all { 0 } else { layers })..=layers {
        let refs = b.ue()?;
        ensure!(refs <= 7, "HEVC decoded picture budget");
        let reorder = b.ue()?;
        ensure!(reorder <= refs, "HEVC reorder budget");
        max_num_reorder_pics = max_num_reorder_pics.max(reorder);
        b.ue()?;
    }
    let min_cb = b.ue()? + 3;
    let diff = b.ue()?;
    ensure!(
        min_cb <= 6 && diff <= 3 && min_cb + diff <= 6,
        "HEVC coding tree budget"
    );
    Ok(SequenceInfo {
        width,
        height,
        max_num_reorder_pics,
    })
}
pub struct Config {
    pub annex_b: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub max_num_reorder_pics: u32,
}
pub fn config(data: &[u8]) -> Result<Config> {
    ensure!(
        (117..=65536).contains(&data.len()) && data.get(4..8) == Some(b"hvc1"),
        "HEVC sample entry"
    );
    let size = u32::from_be_bytes(data[..4].try_into()?) as usize;
    ensure!(size == data.len(), "HEVC sample entry length");
    let mut pos = 86;
    let mut record = None;
    let mut boxes = 0;
    while pos < size {
        // AirPlay sample descriptions may end with a short zero-alignment tail.
        // It is outside hvcC; never treat a partial nonzero box as padding.
        if size - pos < 8 {
            ensure!(
                data[pos..].iter().all(|&v| v == 0),
                "HEVC nonzero container tail: offset={pos} remaining={}",
                size - pos
            );
            break;
        }
        let n = u32::from_be_bytes(data[pos..pos + 4].try_into()?) as usize;
        ensure!(n >= 8 && n <= size - pos, "HEVC child box length");
        if &data[pos + 4..pos + 8] == b"hvcC" {
            ensure!(record.is_none(), "duplicate hvcC");
            record = Some(&data[pos + 8..pos + n]);
        }
        pos += n;
        boxes += 1;
        ensure!(boxes <= 8, "HEVC child box budget");
    }
    let r = record.ok_or_else(|| anyhow::anyhow!("missing hvcC"))?;
    ensure!(
        r.len() >= 23 && r[0] == 1 && r[1] & 31 == 1,
        "HEVC configuration profile"
    );
    ensure!(
        r[16] & 3 == 1 && r[17] & 7 == 0 && r[18] & 7 == 0,
        "HEVC configuration chroma/depth"
    );
    ensure!(
        r[21] & 3 == 3 && (1..=8).contains(&r[22]),
        "HEVC configuration arrays/lengths"
    );
    let mut p = 23;
    let mut seen = 0;
    let mut size = None;
    let mut max_num_reorder_pics = 0;
    let mut out = Vec::new();
    for _ in 0..r[22] {
        ensure!(r.len() - p >= 3, "truncated HEVC parameter array");
        let t = r[p] & 63;
        ensure!((32..=34).contains(&t), "HEVC parameter type");
        let count = u16::from_be_bytes(r[p + 1..p + 3].try_into()?) as usize;
        p += 3;
        ensure!((1..=8).contains(&count), "HEVC parameter count");
        for _ in 0..count {
            ensure!(r.len() - p >= 2, "truncated HEVC parameter length");
            let n = u16::from_be_bytes(r[p..p + 2].try_into()?) as usize;
            p += 2;
            ensure!(
                (2..=16384).contains(&n) && n <= r.len() - p,
                "HEVC parameter length"
            );
            let nal = &r[p..p + n];
            ensure!(kind(nal)? == t, "HEVC parameter NAL mismatch");
            if t == 33 {
                let info = sequence_info(nal)?;
                let next = (info.width, info.height);
                ensure!(
                    size.is_none_or(|old| old == next),
                    "HEVC parameter dimensions disagree"
                );
                size = Some(next);
                max_num_reorder_pics = max_num_reorder_pics.max(info.max_num_reorder_pics);
            }
            out.extend_from_slice(&[0, 0, 0, 1]);
            out.extend_from_slice(nal);
            p += n;
        }
        seen |= 1 << (t - 32);
    }
    ensure!(seen == 7 && p == r.len(), "incomplete HEVC configuration");
    let (width, height) = size.ok_or_else(|| anyhow::anyhow!("missing HEVC SPS dimensions"))?;
    Ok(Config {
        annex_b: out,
        width,
        height,
        max_num_reorder_pics,
    })
}
pub fn annex_b(data: &mut [u8]) -> Result<()> {
    ensure!(
        !data.is_empty() && data.len() <= MAX_FRAME,
        "HEVC frame budget"
    );
    let mut p = 0;
    let mut count = 0;
    while p < data.len() {
        ensure!(data.len() - p >= 4, "truncated HEVC NAL length");
        let n = u32::from_be_bytes(data[p..p + 4].try_into()?) as usize;
        ensure!(n >= 2 && n <= data.len() - p - 4, "HEVC NAL length");
        let nal = &data[p + 4..p + 4 + n];
        let t = kind(nal)?;
        ensure!(t <= 40, "unsupported HEVC NAL type");
        if t == 33 {
            dimensions(nal)?;
        }
        data[p..p + 4].copy_from_slice(&[0, 0, 0, 1]);
        p += 4 + n;
        count += 1;
        ensure!(count <= 4096, "HEVC NAL count");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Bits(Vec<bool>);
    impl Bits {
        fn n(&mut self, v: u64, n: usize) {
            for i in (0..n).rev() {
                self.0.push((v >> i) & 1 != 0);
            }
        }
        fn ue(&mut self, v: u32) {
            let v = u64::from(v) + 1;
            let n = 64 - v.leading_zeros() as usize;
            self.n(0, n - 1);
            self.n(v, n);
        }
        fn finish(mut self) -> Vec<u8> {
            self.n(1, 1);
            while !self.0.len().is_multiple_of(8) {
                self.n(0, 1);
            }
            let mut result = vec![0x42, 1];
            let mut zeros = 0;
            for bits in self.0.chunks(8) {
                let v = bits.iter().fold(0u8, |v, b| (v << 1) | u8::from(*b));
                if zeros >= 2 && v <= 3 {
                    result.push(3);
                    zeros = 0;
                }
                result.push(v);
                zeros = if v == 0 { zeros + 1 } else { 0 };
            }
            result
        }
    }
    fn sps(w: u32, h: u32, refs: u32, depth: u32) -> Vec<u8> {
        sps_with_reorder(w, h, refs, depth, 0)
    }
    fn sps_with_reorder(w: u32, h: u32, refs: u32, depth: u32, reorder: u32) -> Vec<u8> {
        let mut b = Bits(vec![]);
        b.n(0, 4);
        b.n(0, 3);
        b.n(1, 1);
        b.n(0, 2);
        b.n(0, 1);
        b.n(1, 5);
        b.n(1 << 30, 32);
        b.n(0, 48);
        b.n(150, 8);
        b.ue(0);
        b.ue(1);
        b.ue(w);
        b.ue(h);
        b.n(0, 1);
        b.ue(depth);
        b.ue(depth);
        b.ue(4);
        b.n(0, 1);
        b.ue(refs - 1);
        b.ue(reorder);
        b.ue(0);
        b.ue(0);
        b.ue(3);
        b.finish()
    }
    fn entry(sps: &[u8]) -> Vec<u8> {
        let mut r = vec![0; 23];
        r[0] = 1;
        r[1] = 1;
        r[12] = 150;
        r[16] = 0xfd;
        r[17] = 0xf8;
        r[18] = 0xf8;
        r[21] = 3;
        r[22] = 3;
        for (kind, nal) in [
            (32, &[0x40, 1, 0x0c][..]),
            (33, sps),
            (34, &[0x44, 1, 0][..]),
        ] {
            r.extend_from_slice(&[0x80 | kind, 0, 1]);
            r.extend_from_slice(&(nal.len() as u16).to_be_bytes());
            r.extend_from_slice(nal);
        }
        let mut data = vec![0; 86];
        data[4..8].copy_from_slice(b"hvc1");
        data.extend_from_slice(&((r.len() + 8) as u32).to_be_bytes());
        data.extend_from_slice(b"hvcC");
        data.extend_from_slice(&r);
        let size = data.len() as u32;
        data[..4].copy_from_slice(&size.to_be_bytes());
        data
    }
    #[test]
    fn bounded_4k_and_portrait_sps() {
        assert_eq!(dimensions(&sps(3840, 2160, 8, 0)).unwrap(), (3840, 2160));
        assert_eq!(dimensions(&sps(996, 2160, 4, 0)).unwrap(), (996, 2160));
        assert_eq!(dimensions(&sps(2560, 1440, 4, 0)).unwrap(), (2560, 1440));
        for (w, h, refs, depth) in [
            (3841, 2160, 4, 0),
            (3840, 2161, 4, 0),
            (1, 2160, 4, 0),
            (3840, 2160, 9, 0),
            (3840, 2160, 4, 2),
        ] {
            assert!(dimensions(&sps(w, h, refs, depth)).is_err());
        }
    }
    #[test]
    fn configuration_reports_validated_reorder_requirement() {
        for reorder in [0, 1, 3] {
            let parsed = config(&entry(&sps_with_reorder(996, 2160, 4, 0, reorder))).unwrap();
            assert_eq!((parsed.width, parsed.height), (996, 2160));
            assert_eq!(parsed.max_num_reorder_pics, reorder);
        }
        assert!(config(&entry(&sps_with_reorder(996, 2160, 4, 0, 4))).is_err());
    }
    #[test]
    fn bounded_boxes_arrays_and_truncations() {
        let data = entry(&sps(3840, 2160, 4, 0));
        assert!(config(&data).is_ok());
        for end in 0..data.len() {
            assert!(config(&data[..end]).is_err());
        }
        for (offset, value) in [
            (0, 255),
            (86, 255),
            (94, 2),
            (95, 2),
            (110, 254),
            (111, 250),
            (115, 0),
            (116, 255),
            (118, 255),
            (120, 255),
        ] {
            let mut bad = data.clone();
            bad[offset] = value;
            assert!(config(&bad).is_err(), "offset {offset}");
        }
        assert!(config(&entry(&sps(3840, 2161, 4, 0))).is_err());
    }
    #[test]
    fn sample_entry_alignment_is_bounded_and_zero_only() {
        let original = entry(&sps(996, 2160, 4, 0));
        for n in 1..8 {
            let mut data = original.clone();
            data.resize(data.len() + n, 0);
            let len = data.len() as u32;
            data[..4].copy_from_slice(&len.to_be_bytes());
            assert!(config(&data).is_ok());
            *data.last_mut().unwrap() = 1;
            assert!(config(&data).is_err());
        }
    }
    #[test]
    fn access_unit_lengths_and_layer_rejection() {
        let nal = sps(996, 2160, 4, 0);
        let mut data = (nal.len() as u32).to_be_bytes().to_vec();
        data.extend_from_slice(&nal);
        assert!(annex_b(&mut data).is_ok());
        assert_eq!(&data[..4], &[0, 0, 0, 1]);
        for mut bad in [
            vec![],
            vec![0, 0, 0],
            vec![255, 255, 255, 255, 0x26, 1],
            vec![0, 0, 0, 2, 0x26, 0],
            vec![0, 0, 0, 2, 0x27, 1],
            vec![0, 0, 0, 2, 0xa6, 1],
        ] {
            assert!(annex_b(&mut bad).is_err());
        }
    }
    #[test]
    fn embedded_annex_b_cannot_bypass_parameter_validation() {
        let mut packet = vec![0, 0, 0, 7, 0x26, 1, 0, 0, 1, 0x42, 1];
        assert!(annex_b(&mut packet).is_err());
        let mut bad = sps(3840, 2160, 4, 0);
        bad.extend_from_slice(&[0, 0, 3, 255]);
        assert!(dimensions(&bad).is_err());
    }
    #[test]
    fn deterministic_malformed_hevc_never_panics() {
        let seed = entry(&sps(3840, 2160, 4, 0));
        let mut state = 0xbabefaceu32;
        for _ in 0..8192 {
            let mut bytes = seed.clone();
            for _ in 0..3 {
                state = state.wrapping_mul(1664525).wrapping_add(1013904223);
                let p = state as usize % bytes.len();
                bytes[p] = (state >> 24) as u8;
            }
            let _ = config(&bytes);
            let _ = dimensions(&bytes);
            let _ = annex_b(&mut bytes);
        }
    }
}
