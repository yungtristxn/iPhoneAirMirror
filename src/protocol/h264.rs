use anyhow::{Result, ensure};
pub const MAX_FRAME: usize = 2 * 1024 * 1024;
// Reject hostile decoded dimensions/reference budgets before handing an SPS to an OS codec.
pub fn dimensions(nal: &[u8]) -> Result<(u32, u32)> {
    ensure!(
        nal.len() >= 5 && nal.len() <= 16384 && nal[0] & 31 == 7,
        "SPS size/type"
    );
    ensure!(
        !nal[1..].windows(3).any(|w| w == [0, 0, 1]),
        "embedded H.264 start code"
    );
    let mut rbsp = Vec::with_capacity(nal.len());
    let mut zeros = 0;
    for (index, &b) in nal[1..].iter().enumerate() {
        if zeros >= 2 && b == 3 {
            ensure!(
                nal.get(index + 2).is_some_and(|&next| next <= 3),
                "invalid H.264 emulation prevention"
            );
            zeros = 0;
            continue;
        }
        rbsp.push(b);
        zeros = if b == 0 { zeros + 1 } else { 0 };
    }
    struct Bits<'a> {
        data: &'a [u8],
        bit: usize,
    }
    impl Bits<'_> {
        fn n(&mut self, n: usize) -> Result<u32> {
            ensure!(
                n <= 32 && self.bit + n <= self.data.len() * 8,
                "truncated SPS"
            );
            let mut v = 0;
            for _ in 0..n {
                v = (v << 1) | u32::from((self.data[self.bit / 8] >> (7 - self.bit % 8)) & 1);
                self.bit += 1;
            }
            Ok(v)
        }
        fn ue(&mut self) -> Result<u32> {
            let mut z = 0;
            while self.n(1)? == 0 {
                z += 1;
                ensure!(z <= 24, "SPS Golomb overflow");
            }
            Ok((1 << z) - 1 + self.n(z)?)
        }
        fn se(&mut self) -> Result<i32> {
            let v = self.ue()?;
            Ok(if v & 1 != 0 {
                v.div_ceil(2) as i32
            } else {
                -((v / 2) as i32)
            })
        }
    }
    let mut b = Bits {
        data: &rbsp,
        bit: 0,
    };
    let profile = b.n(8)?;
    b.n(8)?;
    ensure!(b.n(8)? <= 51, "H.264 level");
    ensure!(b.ue()? <= 31, "SPS ID");
    if matches!(
        profile,
        100 | 110 | 122 | 244 | 44 | 83 | 86 | 118 | 128 | 138 | 139 | 134 | 135
    ) {
        ensure!(b.ue()? == 1, "only 4:2:0 H.264");
        ensure!(b.ue()? == 0 && b.ue()? == 0, "only 8-bit SDR H.264");
        b.n(1)?;
        if b.n(1)? != 0 {
            for i in 0..8 {
                if b.n(1)? != 0 {
                    let mut last = 8i32;
                    let mut next = 8i32;
                    for _ in 0..if i < 6 { 16 } else { 64 } {
                        if next != 0 {
                            next = (last + b.se()?).rem_euclid(256);
                        }
                        if next != 0 {
                            last = next;
                        }
                    }
                }
            }
        }
    } else {
        ensure!(matches!(profile, 66 | 77 | 88), "H.264 profile");
    }
    ensure!(b.ue()? <= 12, "frame number bits");
    let poc = b.ue()?;
    match poc {
        0 => {
            ensure!(b.ue()? <= 12, "POC bits");
        }
        1 => {
            b.n(1)?;
            b.se()?;
            b.se()?;
            let count = b.ue()?;
            ensure!(count <= 256, "POC cycle budget");
            for _ in 0..count {
                b.se()?;
            }
        }
        2 => {}
        _ => anyhow::bail!("POC type"),
    }
    ensure!(b.ue()? <= 8, "H.264 reference frame budget");
    b.n(1)?;
    let wmb = b.ue()? + 1;
    let hmb = b.ue()? + 1;
    ensure!(wmb <= 120 && hmb <= 120, "H.264 coded dimensions");
    ensure!(wmb * hmb <= 120 * 68, "H.264 coded pixel budget");
    ensure!(b.n(1)? == 1, "interlaced video unsupported");
    b.n(1)?;
    let (mut left, mut right, mut top, mut bottom) = (0, 0, 0, 0);
    if b.n(1)? != 0 {
        left = b.ue()?;
        right = b.ue()?;
        top = b.ue()?;
        bottom = b.ue()?;
    }
    let width = (wmb * 16)
        .checked_sub((left + right) * 2)
        .ok_or_else(|| anyhow::anyhow!("SPS horizontal crop"))?;
    let height = (hmb * 16)
        .checked_sub((top + bottom) * 2)
        .ok_or_else(|| anyhow::anyhow!("SPS vertical crop"))?;
    ensure!(
        width > 0 && height > 0 && width * height <= 1920 * 1080,
        "H.264 display dimensions"
    );
    Ok((width, height))
}
pub fn avcc_config(data: &[u8]) -> Result<Vec<u8>> {
    ensure!(
        (7..=65536).contains(&data.len()) && data[0] == 1 && data[4] & 3 == 3,
        "H.264 configuration"
    );
    let mut p = 6;
    let nsps = data[5] & 31;
    ensure!(nsps > 0 && nsps <= 8, "SPS count");
    let mut out = Vec::new();
    fn nal(data: &[u8], p: &mut usize, out: &mut Vec<u8>, kind: u8) -> Result<()> {
        let bytes = data
            .get(*p..*p + 2)
            .ok_or_else(|| anyhow::anyhow!("truncated parameter length"))?;
        *p += 2;
        let len = u16::from_be_bytes(bytes.try_into()?) as usize;
        ensure!(len > 0 && len <= 16384, "parameter length");
        let bytes = data
            .get(*p..*p + len)
            .ok_or_else(|| anyhow::anyhow!("truncated parameter NAL"))?;
        ensure!(
            !bytes[1..].windows(3).any(|w| w == [0, 0, 1]),
            "embedded H.264 parameter start code"
        );
        ensure!(
            bytes[0] & 0x80 == 0 && bytes[0] & 31 == kind,
            "parameter NAL type"
        );
        *p += len;
        if kind == 7 {
            dimensions(bytes)?;
        }
        out.extend_from_slice(&[0, 0, 0, 1]);
        out.extend_from_slice(bytes);
        Ok(())
    }
    for _ in 0..nsps {
        nal(data, &mut p, &mut out, 7)?;
    }
    let npps = *data
        .get(p)
        .ok_or_else(|| anyhow::anyhow!("missing PPS count"))?;
    p += 1;
    ensure!(npps > 0 && npps <= 8, "PPS count");
    for _ in 0..npps {
        nal(data, &mut p, &mut out, 8)?;
    }
    Ok(out)
}
pub fn annex_b(data: &mut [u8]) -> Result<bool> {
    ensure!(
        !data.is_empty() && data.len() <= MAX_FRAME,
        "H.264 frame size"
    );
    let mut p = 0;
    let mut idr = false;
    let mut nals = 0;
    while p < data.len() {
        let header = data
            .get(p..p + 4)
            .ok_or_else(|| anyhow::anyhow!("truncated NAL length"))?;
        let n = u32::from_be_bytes(header.try_into()?) as usize;
        ensure!(n > 0 && n <= data.len() - p - 4, "NAL length");
        ensure!(data[p + 4] & 0x80 == 0, "forbidden NAL bit");
        ensure!(
            !data[p + 5..p + 4 + n].windows(3).any(|w| w == [0, 0, 1]),
            "embedded H.264 start code"
        );
        let kind = data[p + 4] & 31;
        ensure!((1..=12).contains(&kind), "unsupported NAL type");
        idr |= kind == 5;
        if kind == 7 {
            dimensions(&data[p + 4..p + 4 + n])?;
        }
        data[p..p + 4].copy_from_slice(&[0, 0, 0, 1]);
        p += 4 + n;
        nals += 1;
        ensure!(nals <= 4096, "NAL count");
    }
    Ok(idr)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn codec_lengths() {
        let sps = test_sps(120, 68, 4);
        let mut valid = vec![1, 66, 0, 31, 255, 225, 0, sps.len() as u8];
        valid.extend_from_slice(&sps);
        valid.extend_from_slice(&[1, 0, 2, 104, 1]);
        assert!(avcc_config(&valid).is_ok());
        for i in 0..valid.len() {
            assert!(avcc_config(&valid[..i]).is_err());
        }
        let mut bad = valid;
        bad[6] = 255;
        assert!(avcc_config(&bad).is_err());
    }
    fn test_sps(w: u32, h: u32, crop: u32) -> Vec<u8> {
        let mut bits = String::new();
        let mut ue = |v: u32| {
            let s = format!("{:b}", v + 1);
            bits.push_str(&"0".repeat(s.len() - 1));
            bits.push_str(&s);
        };
        ue(0);
        ue(0);
        ue(0);
        ue(0);
        ue(1);
        bits.push('0');
        for v in [w - 1, h - 1] {
            let s = format!("{:b}", v + 1);
            bits.push_str(&"0".repeat(s.len() - 1));
            bits.push_str(&s);
        }
        bits.push_str("111");
        for v in [0, 0, 0, crop] {
            let s = format!("{:b}", v + 1);
            bits.push_str(&"0".repeat(s.len() - 1));
            bits.push_str(&s);
        }
        bits.push('1');
        while !bits.len().is_multiple_of(8) {
            bits.push('0');
        }
        let mut nal = vec![0x67, 66, 0, 31];
        nal.extend(
            bits.as_bytes()
                .chunks(8)
                .map(|c| c.iter().fold(0u8, |v, b| (v << 1) | (b - b'0'))),
        );
        nal
    }
    #[test]
    fn decoded_dimension_limits() {
        assert_eq!(dimensions(&test_sps(120, 68, 4)).unwrap(), (1920, 1080));
        assert!(dimensions(&test_sps(121, 68, 4)).is_err());
        assert!(dimensions(&test_sps(120, 120, 0)).is_err());
        assert!(dimensions(&test_sps(120, 120, 420)).is_err());
    }
    #[test]
    fn deterministic_mutated_codec_records_do_not_panic() {
        let seed = test_sps(120, 68, 4);
        let mut state = 0x12345678u32;
        for _ in 0..8192 {
            let mut s = seed.clone();
            for _ in 0..3 {
                state = state.wrapping_mul(1664525).wrapping_add(1013904223);
                let p = state as usize % s.len();
                s[p] = (state >> 24) as u8;
            }
            let _ = dimensions(&s);
            let _ = avcc_config(&s);
            let _ = annex_b(&mut s);
        }
    }
    #[test]
    fn nal_lengths() {
        for mut v in [
            vec![0, 0, 0],
            vec![255, 255, 255, 255, 101],
            vec![0, 0, 0, 0],
            vec![0, 0, 0, 2, 101],
        ] {
            assert!(annex_b(&mut v).is_err());
        }
        let mut good = [0, 0, 0, 1, 101];
        assert!(annex_b(&mut good).unwrap());
    }
    #[test]
    fn embedded_annex_b_cannot_hide_sps() {
        let mut packet = vec![0, 0, 0, 6, 0x41, 0, 0, 1, 0x67, 0];
        assert!(annex_b(&mut packet).is_err());
        let mut bad = test_sps(120, 68, 4);
        bad.extend_from_slice(&[0, 0, 3, 255]);
        assert!(dimensions(&bad).is_err());
    }
}
