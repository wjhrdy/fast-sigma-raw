//! Lossless JPEG (ITU-T T.81 process 14 / LJ92) for 16-bit LinearRaw DNG.
//!
//! Adapted from x3fuse-core's Apache-2.0-licensed DNG encoder at commit
//! 4435690328429a25ec9436ff750c01ce8fe95dba. This version accepts strided
//! image storage and recomputes differences during its second pass instead of
//! retaining a full-frame difference buffer.

/// Encode a strided, interleaved u16 raster as one standalone SOI..EOI
/// lossless-JPEG stream. Predictor 1 (left sample), point transform 0, and one
/// optimal DC Huffman table are used for all components.
pub(crate) fn encode(
    samples: &[u16],
    width: usize,
    height: usize,
    components: usize,
    row_stride: usize,
) -> Option<Vec<u8>> {
    if width == 0
        || width > u16::MAX as usize
        || height == 0
        || height > u16::MAX as usize
        || components == 0
        || components > 4
    {
        return None;
    }
    let row_samples = width.checked_mul(components)?;
    if row_stride < row_samples {
        return None;
    }
    let required = (height - 1)
        .checked_mul(row_stride)?
        .checked_add(row_samples)?;
    if samples.len() < required {
        return None;
    }

    let mut frequencies = [0_u64; 17];
    for row in 0..height {
        for column in 0..width {
            let offset = row * row_stride + column * components;
            for component in 0..components {
                let prediction = predict(
                    samples, offset, row, column, component, components, row_stride,
                );
                let difference = samples[offset + component].wrapping_sub(prediction);
                frequencies[category(difference) as usize] += 1;
            }
        }
    }

    let table = HuffmanTable::optimal(&frequencies);
    let sample_count = width.checked_mul(height)?.checked_mul(components)?;
    let mut output = Vec::with_capacity(sample_count);
    output.extend_from_slice(&[0xff, 0xd8]);
    write_dht(&mut output, &table);
    write_sof3(&mut output, width as u16, height as u16, components as u8);
    write_sos(&mut output, components as u8);

    let mut bits = BitWriter::new(output);
    for row in 0..height {
        for column in 0..width {
            let offset = row * row_stride + column * components;
            for component in 0..components {
                let prediction = predict(
                    samples, offset, row, column, component, components, row_stride,
                );
                let difference = samples[offset + component].wrapping_sub(prediction);
                let size = category(difference);
                let (code, length) = table.codes[size as usize];
                bits.put(code as u32, length);
                if size > 0 && size < 16 {
                    let signed = difference as i16 as i32;
                    let value = if signed > 0 { signed } else { signed - 1 };
                    bits.put((value as u32) & ((1 << size) - 1), size as u32);
                }
            }
        }
    }
    let mut output = bits.finish();
    output.extend_from_slice(&[0xff, 0xd9]);
    Some(output)
}

#[inline]
fn predict(
    samples: &[u16],
    offset: usize,
    row: usize,
    column: usize,
    component: usize,
    components: usize,
    row_stride: usize,
) -> u16 {
    if column > 0 {
        samples[offset + component - components]
    } else if row > 0 {
        samples[offset + component - row_stride]
    } else {
        1 << 15
    }
}

fn category(difference: u16) -> u8 {
    if difference == 0 {
        return 0;
    }
    if difference == 0x8000 {
        return 16;
    }
    let magnitude = (difference as i16 as i32).unsigned_abs();
    (32 - magnitude.leading_zeros()) as u8
}

struct HuffmanTable {
    bits: [u8; 16],
    values: Vec<u8>,
    codes: [(u16, u32); 17],
}

impl HuffmanTable {
    /// Annex K.2 length-limited optimal-table construction. A reserved
    /// pseudo-symbol prevents an all-ones code from representing real data.
    fn optimal(input: &[u64; 17]) -> Self {
        const RESERVED: usize = 17;
        let mut frequency = [0_u64; 18];
        frequency[..17].copy_from_slice(input);
        frequency[RESERVED] = 1;

        let mut code_size = [0_usize; 18];
        let mut others = [-1_isize; 18];
        loop {
            let first = least_frequent(&frequency, None);
            let Some(first) = first else { break };
            let second = least_frequent(&frequency, Some(first));
            let Some(second) = second else { break };

            frequency[first] += frequency[second];
            frequency[second] = 0;
            increment_tree(first, &mut code_size, &others);
            let mut tail = first;
            while others[tail] >= 0 {
                tail = others[tail] as usize;
            }
            others[tail] = second as isize;
            increment_tree(second, &mut code_size, &others);
        }

        let mut lengths = [0_i32; 33];
        for &size in &code_size {
            if size > 0 {
                lengths[size] += 1;
            }
        }
        for length in (17..=32).rev() {
            while lengths[length] > 0 {
                let mut shorter = length - 2;
                while lengths[shorter] == 0 {
                    shorter -= 1;
                }
                lengths[length] -= 2;
                lengths[length - 1] += 1;
                lengths[shorter + 1] += 2;
                lengths[shorter] -= 1;
            }
        }
        for length in (1..=16).rev() {
            if lengths[length] > 0 {
                lengths[length] -= 1;
                break;
            }
        }

        let mut bits = [0_u8; 16];
        for length in 1..=16 {
            bits[length - 1] = lengths[length] as u8;
        }
        let mut ordered: Vec<(usize, usize)> = code_size[..17]
            .iter()
            .enumerate()
            .filter(|(_, size)| **size > 0)
            .map(|(symbol, &size)| (size, symbol))
            .collect();
        ordered.sort_unstable();
        let values: Vec<u8> = ordered.iter().map(|&(_, symbol)| symbol as u8).collect();

        let mut codes = [(0_u16, 0_u32); 17];
        let mut code = 0_u32;
        let mut value_index = 0;
        for (index, &count) in bits.iter().enumerate() {
            for _ in 0..count {
                let symbol = values[value_index] as usize;
                codes[symbol] = (code as u16, index as u32 + 1);
                code += 1;
                value_index += 1;
            }
            code <<= 1;
        }
        debug_assert_eq!(value_index, values.len());

        Self {
            bits,
            values,
            codes,
        }
    }
}

fn least_frequent(frequencies: &[u64; 18], exclude: Option<usize>) -> Option<usize> {
    let mut selected = None;
    let mut value = u64::MAX;
    for (index, &frequency) in frequencies.iter().enumerate() {
        if frequency > 0 && frequency <= value && Some(index) != exclude {
            value = frequency;
            selected = Some(index);
        }
    }
    selected
}

fn increment_tree(mut node: usize, sizes: &mut [usize; 18], others: &[isize; 18]) {
    sizes[node] += 1;
    while others[node] >= 0 {
        node = others[node] as usize;
        sizes[node] += 1;
    }
}

fn write_dht(output: &mut Vec<u8>, table: &HuffmanTable) {
    let length = 2 + 1 + 16 + table.values.len();
    output.extend_from_slice(&[0xff, 0xc4]);
    output.extend_from_slice(&(length as u16).to_be_bytes());
    output.push(0);
    output.extend_from_slice(&table.bits);
    output.extend_from_slice(&table.values);
}

fn write_sof3(output: &mut Vec<u8>, width: u16, height: u16, components: u8) {
    let length = 8 + 3 * components as usize;
    output.extend_from_slice(&[0xff, 0xc3]);
    output.extend_from_slice(&(length as u16).to_be_bytes());
    output.push(16);
    output.extend_from_slice(&height.to_be_bytes());
    output.extend_from_slice(&width.to_be_bytes());
    output.push(components);
    for component in 0..components {
        output.push(component + 1);
        output.push(0x11);
        output.push(0);
    }
}

fn write_sos(output: &mut Vec<u8>, components: u8) {
    let length = 6 + 2 * components as usize;
    output.extend_from_slice(&[0xff, 0xda]);
    output.extend_from_slice(&(length as u16).to_be_bytes());
    output.push(components);
    for component in 0..components {
        output.push(component + 1);
        output.push(0);
    }
    output.push(1);
    output.push(0);
    output.push(0);
}

struct BitWriter {
    output: Vec<u8>,
    accumulator: u32,
    bit_count: u32,
}

impl BitWriter {
    fn new(output: Vec<u8>) -> Self {
        Self {
            output,
            accumulator: 0,
            bit_count: 0,
        }
    }

    fn put(&mut self, value: u32, count: u32) {
        debug_assert!(count <= 16);
        self.accumulator = (self.accumulator << count) | (value & ((1_u32 << count) - 1));
        self.bit_count += count;
        while self.bit_count >= 8 {
            let byte = ((self.accumulator >> (self.bit_count - 8)) & 0xff) as u8;
            self.output.push(byte);
            if byte == 0xff {
                self.output.push(0);
            }
            self.bit_count -= 8;
        }
    }

    fn finish(mut self) -> Vec<u8> {
        if self.bit_count > 0 {
            let padding = 8 - self.bit_count;
            self.put((1 << padding) - 1, padding);
        }
        self.output
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Minimal decoder for the exact LJ92 subset emitted above. Adapted from
    /// x3fuse-core's encoder round-trip tests.
    fn decode(stream: &[u8]) -> (Vec<u16>, usize, usize, usize) {
        assert_eq!(&stream[..2], &[0xff, 0xd8]);
        let mut position = 2;
        let mut lengths = [0_u8; 16];
        let mut values = Vec::new();
        let (mut width, mut height, mut components) = (0, 0, 0);
        loop {
            assert_eq!(stream[position], 0xff);
            let marker = stream[position + 1];
            let segment_length =
                u16::from_be_bytes([stream[position + 2], stream[position + 3]]) as usize;
            let segment = &stream[position + 4..position + 2 + segment_length];
            match marker {
                0xc4 => {
                    assert_eq!(segment[0], 0);
                    lengths.copy_from_slice(&segment[1..17]);
                    values = segment[17..].to_vec();
                }
                0xc3 => {
                    assert_eq!(segment[0], 16);
                    height = u16::from_be_bytes([segment[1], segment[2]]) as usize;
                    width = u16::from_be_bytes([segment[3], segment[4]]) as usize;
                    components = segment[5] as usize;
                }
                0xda => {
                    let scan_components = segment[0] as usize;
                    assert_eq!(scan_components, components);
                    assert_eq!(segment[1 + 2 * scan_components], 1);
                    position += 2 + segment_length;
                    break;
                }
                _ => panic!("unexpected JPEG marker {marker:02x}"),
            }
            position += 2 + segment_length;
        }

        let mut lookup = std::collections::HashMap::new();
        let mut code = 0_u32;
        let mut value_index = 0;
        for (index, &count) in lengths.iter().enumerate() {
            for _ in 0..count {
                lookup.insert((index as u32 + 1, code), values[value_index]);
                code += 1;
                value_index += 1;
            }
            code <<= 1;
        }

        let mut bytes = Vec::new();
        let mut index = position;
        while index < stream.len() {
            if stream[index] == 0xff {
                if stream[index + 1] == 0 {
                    bytes.push(0xff);
                    index += 2;
                    continue;
                }
                assert_eq!(stream[index + 1], 0xd9);
                break;
            }
            bytes.push(stream[index]);
            index += 1;
        }
        let mut bit_position = 0;
        let read_bit = |position: &mut usize| -> u32 {
            let bit = (bytes[*position / 8] >> (7 - *position % 8)) & 1;
            *position += 1;
            bit as u32
        };

        let mut output = vec![0_u16; width * height * components];
        let row_samples = width * components;
        for row in 0..height {
            for column in 0..width {
                for component in 0..components {
                    let (mut length, mut code) = (0, 0);
                    let size = loop {
                        code = (code << 1) | read_bit(&mut bit_position);
                        length += 1;
                        assert!(length <= 16);
                        if let Some(&symbol) = lookup.get(&(length, code)) {
                            break symbol;
                        }
                    };
                    let difference = match size {
                        0 => 0_i32,
                        16 => 32768,
                        _ => {
                            let mut value = 0_i32;
                            for _ in 0..size {
                                value = (value << 1) | read_bit(&mut bit_position) as i32;
                            }
                            if value < (1 << (size - 1)) {
                                value - (1 << size) + 1
                            } else {
                                value
                            }
                        }
                    };
                    let offset = row * row_samples + column * components + component;
                    let prediction = if column > 0 {
                        output[offset - components]
                    } else if row > 0 {
                        output[offset - row_samples]
                    } else {
                        1 << 15
                    };
                    output[offset] = prediction.wrapping_add(difference as u16);
                }
            }
        }
        (output, width, height, components)
    }

    fn round_trip(samples: &[u16], width: usize, height: usize, components: usize) {
        let encoded = encode(samples, width, height, components, width * components).unwrap();
        let (decoded, decoded_width, decoded_height, decoded_components) = decode(&encoded);
        assert_eq!(
            (decoded_width, decoded_height, decoded_components),
            (width, height, components)
        );
        assert_eq!(decoded, samples);
    }

    #[test]
    fn category_boundaries_match_lj92() {
        assert_eq!(category(0), 0);
        assert_eq!(category(1), 1);
        assert_eq!(category(0xffff), 1);
        assert_eq!(category(32767), 15);
        assert_eq!(category(0x8001), 15);
        assert_eq!(category(0x8000), 16);
    }

    #[test]
    fn encoder_accepts_strided_three_component_data() {
        let samples = [1, 2, 3, 4, 5, 6, 99, 7, 8, 9, 10, 11, 12, 99];
        let encoded = encode(&samples, 2, 2, 3, 7).unwrap();
        assert_eq!(&encoded[..2], &[0xff, 0xd8]);
        assert_eq!(&encoded[encoded.len() - 2..], &[0xff, 0xd9]);
        let sof = encoded
            .windows(2)
            .position(|marker| marker == [0xff, 0xc3])
            .unwrap();
        assert_eq!(u16::from_be_bytes([encoded[sof + 5], encoded[sof + 6]]), 2);
        assert_eq!(u16::from_be_bytes([encoded[sof + 7], encoded[sof + 8]]), 2);
    }

    #[test]
    fn bit_writer_stuffs_ff_bytes() {
        let mut writer = BitWriter::new(Vec::new());
        writer.put(0xff, 8);
        writer.put(0xab, 8);
        assert_eq!(writer.finish(), vec![0xff, 0, 0xab]);
    }

    #[test]
    fn round_trips_full_range_noise() {
        let (width, height, components) = (64, 16, 3);
        let mut state = 0x1234_5678_u32;
        let samples: Vec<u16> = (0..width * height * components)
            .map(|_| {
                state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                (state >> 16) as u16
            })
            .collect();
        round_trip(&samples, width, height, components);
    }

    #[test]
    fn round_trips_constant_and_extreme_images() {
        round_trip(&vec![512; 24 * 3 * 3], 24, 3, 3);
        let samples: Vec<u16> = (0..16 * 4 * 3)
            .map(|index| if index % 2 == 0 { 0 } else { u16::MAX })
            .collect();
        round_trip(&samples, 16, 4, 3);
    }
}
