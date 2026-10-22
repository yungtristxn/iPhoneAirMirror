//! Explicit negotiated media ciphers. Authentication failure never selects another cipher.
//! Wire facts: WirelessCarPlay 51145ef55f8dd9f1cbadd58353cacb5e0ca215e9,
//! AirPlayReceiverSessionScreen.c and AirPlayReceiverSession.c; see VERIFIED_PAIRING.md.
use super::{hap::SessionSecret, pairing};
use aes::cipher::{BlockDecryptMut, KeyIvInit as _, StreamCipher, block_padding::NoPadding};
use anyhow::{Result, ensure};
use chacha20poly1305::{ChaCha20Poly1305, KeyInit, Nonce, Tag, aead::AeadInPlace};
use zeroize::{Zeroize, Zeroizing};

pub enum Security {
    Legacy {
        key: Zeroizing<[u8; 16]>,
        iv: Zeroizing<[u8; 16]>,
    },
    Hap(SessionSecret),
}
impl Security {
    pub fn legacy(key: [u8; 16], iv: [u8; 16]) -> Self {
        Self::Legacy {
            key: Zeroizing::new(key),
            iv: Zeroizing::new(iv),
        }
    }
    pub fn event_keys(&self) -> Result<Option<super::secure::RecordKeys>> {
        match self {
            Self::Legacy { .. } => Ok(None),
            Self::Hap(secret) => Ok(Some(super::secure::RecordKeys::event_receiver(
                secret.bytes(),
            )?)),
        }
    }
    pub fn video(&self, id: u64) -> Result<VideoCipher> {
        Ok(match self {
            Self::Legacy { key, .. } => {
                VideoCipher::Legacy(Box::new(pairing::video_cipher(key, id)))
            }
            Self::Hap(secret) => {
                let key = Zeroizing::new(secret.stream_key(id)?);
                VideoCipher::Hap {
                    cipher: ChaCha20Poly1305::new_from_slice(key.as_ref())
                        .map_err(|_| anyhow::anyhow!("screen key length"))?,
                    counter: 0,
                    failed: false,
                }
            }
        })
    }
    pub fn audio(
        &self,
        stream_id: Option<u64>,
        direct_key: Option<[u8; 32]>,
    ) -> Result<AudioCipher> {
        Ok(match self {
            Self::Legacy { key, iv } => {
                ensure!(direct_key.is_none(), "modern audio key on legacy session");
                AudioCipher::Legacy {
                    key: key.clone(),
                    iv: iv.clone(),
                }
            }
            Self::Hap(secret) => {
                let key =
                    Zeroizing::new(match direct_key {
                        Some(key) => key,
                        None => secret.stream_key(stream_id.ok_or_else(|| {
                            anyhow::anyhow!("HAP audio stream key or ID required")
                        })?)?,
                    });
                AudioCipher::Hap(
                    ChaCha20Poly1305::new_from_slice(key.as_ref())
                        .map_err(|_| anyhow::anyhow!("audio key length"))?,
                )
            }
        })
    }
}

pub enum VideoCipher {
    Legacy(Box<pairing::AesCtr>),
    Hap {
        cipher: ChaCha20Poly1305,
        counter: u64,
        failed: bool,
    },
}
impl VideoCipher {
    pub fn overhead(&self) -> usize {
        if matches!(self, Self::Hap { .. }) {
            16
        } else {
            0
        }
    }
    pub fn open(&mut self, header: &[u8; 128], payload: &mut Vec<u8>) -> Result<()> {
        match self {
            Self::Legacy(cipher) => {
                cipher.apply_keystream(payload);
                Ok(())
            }
            Self::Hap {
                cipher,
                counter,
                failed,
            } => {
                ensure!(
                    !*failed && *counter != u64::MAX,
                    "screen cipher closed or nonce exhausted"
                );
                *failed = true;
                ensure!(
                    payload.len() > 16
                        && u32::from_le_bytes(header[..4].try_into()?) as usize == payload.len(),
                    "authenticated screen payload length"
                );
                let size = payload.len() - 16;
                let mut nonce = [0u8; 12];
                nonce[4..].copy_from_slice(&counter.to_le_bytes());
                let tag: [u8; 16] = payload[size..].try_into()?;
                if cipher
                    .decrypt_in_place_detached(
                        Nonce::from_slice(&nonce),
                        header,
                        &mut payload[..size],
                        Tag::from_slice(&tag),
                    )
                    .is_err()
                {
                    payload.zeroize();
                    anyhow::bail!("screen authentication failed");
                }
                payload.truncate(size);
                *counter += 1;
                *failed = false;
                Ok(())
            }
        }
    }
}
pub enum AudioCipher {
    Legacy {
        key: Zeroizing<[u8; 16]>,
        iv: Zeroizing<[u8; 16]>,
    },
    Hap(ChaCha20Poly1305),
}
impl AudioCipher {
    /// Strip retransmission wrappers before calling. Authenticate timestamp/SSRC
    /// and payload before untrusted sequence metadata may influence the jitter queue.
    pub fn open_packet(&self, packet: &mut [u8]) -> Result<usize> {
        ensure!(
            packet.len() > 12 && packet.len() <= 8192 && packet[0] == 0x80,
            "RTP audio framing"
        );
        match self {
            Self::Legacy { key, iv } => {
                let encrypted = (packet.len() - 12) / 16 * 16;
                cbc::Decryptor::<aes::Aes128>::new_from_slices(key.as_ref(), iv.as_ref())
                    .map_err(|_| anyhow::anyhow!("audio key length"))?
                    .decrypt_padded_mut::<NoPadding>(&mut packet[12..12 + encrypted])
                    .map_err(|_| anyhow::anyhow!("audio ciphertext length"))?;
                Ok(packet.len())
            }
            Self::Hap(cipher) => {
                ensure!(
                    packet.len() > 12 + 16 + 8,
                    "authenticated audio payload length"
                );
                let end = packet.len() - 24;
                let aad: [u8; 8] = packet[4..12].try_into()?;
                let tag: [u8; 16] = packet[end..end + 16].try_into()?;
                let mut nonce = [0; 12];
                nonce[4..].copy_from_slice(&packet[end + 16..]);
                if cipher
                    .decrypt_in_place_detached(
                        Nonce::from_slice(&nonce),
                        &aad,
                        &mut packet[12..end],
                        Tag::from_slice(&tag),
                    )
                    .is_err()
                {
                    packet[12..].zeroize();
                    anyhow::bail!("audio authentication failed");
                }
                Ok(end)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn encrypted_video(key: &[u8; 32], counter: u64) -> ([u8; 128], Vec<u8>) {
        let mut header = [0; 128];
        header[..4].copy_from_slice(&20u32.to_le_bytes());
        let mut nonce = [0; 12];
        nonce[4..].copy_from_slice(&counter.to_le_bytes());
        let cipher = ChaCha20Poly1305::new_from_slice(key).unwrap();
        let mut data = vec![1, 2, 3, 4];
        let tag = cipher
            .encrypt_in_place_detached(Nonce::from_slice(&nonce), &header, &mut data)
            .unwrap();
        data.extend_from_slice(&tag);
        (header, data)
    }
    #[test]
    fn screen_binds_full_header_and_nonce_without_cipher_fallback() {
        let key = [7; 32];
        let mut receiver = VideoCipher::Hap {
            cipher: ChaCha20Poly1305::new_from_slice(&key).unwrap(),
            counter: 0,
            failed: false,
        };
        let (header, mut data) = encrypted_video(&key, 0);
        receiver.open(&header, &mut data).unwrap();
        assert_eq!(data, [1, 2, 3, 4]);
        let (header, mut replay) = encrypted_video(&key, 0);
        assert!(receiver.open(&header, &mut replay).is_err());
        let (header, mut next) = encrypted_video(&key, 1);
        assert!(receiver.open(&header, &mut next).is_err());
        for field in [0usize, 5, 40, 127] {
            let mut receiver = VideoCipher::Hap {
                cipher: ChaCha20Poly1305::new_from_slice(&key).unwrap(),
                counter: 0,
                failed: false,
            };
            let (mut header, mut data) = encrypted_video(&key, 0);
            header[field] ^= 1;
            assert!(receiver.open(&header, &mut data).is_err());
        }
        let mut exhausted = VideoCipher::Hap {
            cipher: ChaCha20Poly1305::new_from_slice(&key).unwrap(),
            counter: u64::MAX,
            failed: false,
        };
        let (header, mut data) = encrypted_video(&key, u64::MAX);
        assert!(exhausted.open(&header, &mut data).is_err());
    }
    #[test]
    fn audio_authenticates_payload_and_time_while_allowing_reordering() {
        let key = [4; 32];
        let cipher = ChaCha20Poly1305::new_from_slice(&key).unwrap();
        let receiver = AudioCipher::Hap(cipher.clone());
        for counter in [10u64, 8, 9] {
            let mut header = [0u8; 12];
            header[0] = 0x80;
            header[4..8].copy_from_slice(&24000u32.to_be_bytes());
            let mut nonce = [0; 12];
            nonce[4..].copy_from_slice(&counter.to_le_bytes());
            let mut body = vec![5, 6, 7];
            let tag = cipher
                .encrypt_in_place_detached(Nonce::from_slice(&nonce), &header[4..], &mut body)
                .unwrap();
            let mut packet = header.to_vec();
            packet.extend_from_slice(&body);
            packet.extend_from_slice(&tag);
            packet.extend_from_slice(&nonce[4..]);
            for offset in [4usize, 8, 12, 15, packet.len() - 1] {
                let mut bad = packet.clone();
                bad[offset] ^= 1;
                assert!(receiver.open_packet(&mut bad).is_err());
            }
            assert_eq!(receiver.open_packet(&mut packet).unwrap(), 15);
            assert_eq!(&packet[12..15], [5, 6, 7]);
        }
    }
}
