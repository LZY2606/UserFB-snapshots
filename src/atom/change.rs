use std::borrow::Cow;

use super::*;

pub trait CollectChanges {
    /// Recursively collect changes and return the length difference when applied.
    fn collect_changes<'a>(
        &'a self,
        insert_pos: u64,
        level: u8,
        changes: &mut Vec<Change<'a>>,
    ) -> i64;
}

impl<T: CollectChanges> CollectChanges for Option<T> {
    fn collect_changes<'a>(
        &'a self,
        insert_pos: u64,
        level: u8,
        changes: &mut Vec<Change<'a>>,
    ) -> i64 {
        self.as_ref().map_or(0, |a| a.collect_changes(insert_pos, level, changes))
    }
}

pub trait SimpleCollectChanges: AtomSize + Atom {
    fn state(&self) -> &State;

    /// Add changes, if any, and return the length difference when applied.
    fn existing<'a>(
        &'a self,
        level: u8,
        bounds: &'a AtomBounds,
        changes: &mut Vec<Change<'a>>,
    ) -> i64;

    fn atom_ref(&self) -> AtomRef<'_>;
}

impl<T: SimpleCollectChanges> CollectChanges for T {
    fn collect_changes<'a>(
        &'a self,
        insert_pos: u64,
        level: u8,
        changes: &mut Vec<Change<'a>>,
    ) -> i64 {
        match &self.state() {
            State::Existing(bounds) => {
                let len_diff = self.existing(level + 1, bounds, changes);
                if len_diff != 0 {
                    changes.push(Change::UpdateLen(UpdateAtomLen {
                        bounds,
                        fourcc: Self::FOURCC,
                        len_diff,
                    }));
                }
                len_diff
            }
            State::Remove(bounds) => {
                changes.push(Change::Remove(RemoveAtom { bounds, level: level + 1 }));
                -(bounds.len() as i64)
            }
            State::Replace(bounds) => {
                let len_diff = (self.len() as i64) - (bounds.len() as i64);
                let r = ReplaceAtom { bounds, atom: self.atom_ref(), level: level + 1 };
                changes.push(Change::Replace(r));
                len_diff
            }
            State::Insert => {
                changes.push(Change::Insert(InsertAtom {
                    pos: insert_pos,
                    atom: self.atom_ref(),
                    level: level + 1,
                }));
                self.len() as i64
            }
        }
    }
}

pub trait LeafAtomCollectChanges: SimpleCollectChanges {
    fn state(&self) -> &State;

    fn atom_ref(&self) -> AtomRef<'_>;
}

impl<T: LeafAtomCollectChanges> SimpleCollectChanges for T {
    fn state(&self) -> &State {
        LeafAtomCollectChanges::state(self)
    }

    fn existing<'a>(
        &'a self,
        _level: u8,
        _bounds: &'a AtomBounds,
        _changes: &mut Vec<Change<'a>>,
    ) -> i64 {
        0
    }

    fn atom_ref(&self) -> AtomRef<'_> {
        LeafAtomCollectChanges::atom_ref(self)
    }
}

#[derive(Debug)]
pub enum Change<'a> {
    UpdateLen(UpdateAtomLen<'a>),
    UpdateChunkOffset(UpdateChunkOffsets<'a>),
    Remove(RemoveAtom<'a>),
    Replace(ReplaceAtom<'a>),
    Insert(InsertAtom<'a>),
    RemoveMdat(u64, u64),
    AppendMdat(u64, Vec<u8>),
}

impl std::fmt::Display for Change<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        #[rustfmt::skip]
        match self {
            Change::UpdateLen(UpdateAtomLen { fourcc, .. }) => write!(f, "UpdateLen   {fourcc}  "),
            Change::UpdateChunkOffset(_)                    => write!(f, "UpdateChunkOffset "),
            Change::Remove(_)                               => write!(f, "RemoveAtom        "),
            Change::Replace(r)                              => write!(f, "ReplaceAtom {}  ", r.atom.fourcc()),
            Change::Insert(i)                               => write!(f, "InsertAtom  {}  ", i.atom.fourcc()),
            Change::RemoveMdat(..)                          => write!(f, "RemoveMdat        "),
            Change::AppendMdat(..)                          => write!(f, "AppendMdat        "),
        }?;
        write!(
            f,
            "old_pos: {:6}, old_end: {:6}, len_diff: {:6}, level: {}",
            self.old_pos(),
            self.old_end(),
            self.len_diff(),
            self.level()
        )
    }
}

impl Change<'_> {
    pub fn old_pos(&self) -> u64 {
        match self {
            Self::UpdateLen(c) => c.bounds.pos(),
            Self::UpdateChunkOffset(c) => c.bounds.content_pos() + stco::HEADER_SIZE,
            Self::Remove(c) => c.bounds.pos(),
            Self::Replace(c) => c.bounds.pos(),
            Self::Insert(c) => c.pos,
            Self::RemoveMdat(pos, _) => *pos,
            Self::AppendMdat(pos, _) => *pos,
        }
    }

    pub fn old_end(&self) -> u64 {
        match self {
            Self::UpdateLen(c) => c.bounds.content_pos(),
            Self::UpdateChunkOffset(c) => c.bounds.end(),
            Self::Remove(c) => c.bounds.end(),
            Self::Replace(c) => c.bounds.end(),
            Self::Insert(c) => c.pos,
            Self::RemoveMdat(pos, len) => *pos + *len,
            Self::AppendMdat(pos, _) => *pos,
        }
    }

    pub fn len_diff(&self) -> i64 {
        match self {
            Self::UpdateLen(_) => 0,
            Self::UpdateChunkOffset(_) => 0,
            Self::Remove(c) => -(c.bounds.len() as i64),
            Self::Replace(c) => (c.atom.len() as i64) - (c.bounds.len() as i64),
            Self::Insert(c) => c.atom.len() as i64,
            Self::RemoveMdat(_, len) => -(*len as i64),
            Self::AppendMdat(_, d) => d.len() as i64,
        }
    }

    pub fn level(&self) -> u8 {
        match self {
            Self::UpdateLen(_) => 0,
            Self::UpdateChunkOffset(_) => 6,
            Self::Remove(c) => c.level,
            Self::Replace(c) => c.level,
            Self::Insert(c) => c.level,
            Self::RemoveMdat(_, _) => u8::MAX,
            Self::AppendMdat(_, _) => u8::MAX,
        }
    }
}

#[derive(Debug)]
pub struct UpdateAtomLen<'a> {
    pub bounds: &'a AtomBounds,
    pub fourcc: Fourcc,
    pub len_diff: i64,
}

impl UpdateAtomLen<'_> {
    pub fn update_len(&self, writer: &mut impl Write) -> crate::Result<()> {
        let len = (self.bounds.len() as i64 + self.len_diff) as u64;
        let head = Head::new(self.bounds.ext(), len, self.fourcc);
        head::write(writer, head)?;
        Ok(())
    }
}

#[derive(Debug)]
pub struct RemoveAtom<'a> {
    pub bounds: &'a AtomBounds,
    pub level: u8,
}

#[derive(Debug)]
pub struct ReplaceAtom<'a> {
    pub bounds: &'a AtomBounds,
    pub atom: AtomRef<'a>,
    pub level: u8,
}

#[derive(Debug)]
pub struct InsertAtom<'a> {
    pub pos: u64,
    pub atom: AtomRef<'a>,
    pub level: u8,
}

#[derive(Debug)]
pub struct UpdateChunkOffsets<'a> {
    pub bounds: &'a AtomBounds,
    pub offsets: ChunkOffsets<'a>,
}

#[derive(Debug)]
pub enum ChunkOffsets<'a> {
    Stco(Cow<'a, [u32]>),
    Co64(Cow<'a, [u64]>),
}

impl ChunkOffsets<'_> {
    pub fn update_offsets(
        &self,
        writer: &mut impl Write,
        changes: &[Change<'_>],
    ) -> crate::Result<()> {
        match self {
            ChunkOffsets::Stco(offsets) => write_shifted_offsets(writer, offsets, changes),
            ChunkOffsets::Co64(offsets) => write_shifted_offsets(writer, offsets, changes),
        }
    }
}

pub trait ChunkOffsetInt: Sized + Copy + Into<u64> {
    fn shift(&self, shift: i64) -> crate::Result<Self>;
    fn write(&self, writer: &mut impl Write) -> crate::Result<()>;
}

impl ChunkOffsetInt for u32 {
    fn shift(&self, shift: i64) -> crate::Result<Self> {
        let offset = (*self as i64) + shift;
        u32::try_from(offset).map_err(|_| {
            crate::Error::new(
                ErrorKind::InvalidSampleTable,
                "Sample table chunk offset (stco) does not fit into 32 bits after shifting",
            )
        })
    }

    fn write(&self, writer: &mut impl Write) -> crate::Result<()> {
        writer.write_be_u32(*self)?;
        Ok(())
    }
}
impl ChunkOffsetInt for u64 {
    fn shift(&self, shift: i64) -> crate::Result<Self> {
        let offset = (*self as i64).checked_add(shift).ok_or_else(|| {
            crate::Error::new(
                ErrorKind::InvalidSampleTable,
                "Sample table chunk offset (co64) is out of bounds after shifting",
            )
        })?;
        u64::try_from(offset).map_err(|_| {
            crate::Error::new(
                ErrorKind::InvalidSampleTable,
                "Sample table chunk offset (co64) is out of bounds after shifting",
            )
        })
    }

    fn write(&self, writer: &mut impl Write) -> crate::Result<()> {
        writer.write_be_u64(*self)?;
        Ok(())
    }
}

pub fn write_shifted_offsets<T: ChunkOffsetInt>(
    writer: &mut impl Write,
    offsets: &[T],
    changes: &[Change<'_>],
) -> crate::Result<()> {
    let mut changes_iter = changes.iter().peekable();

    let mut mdat_shift = 0;
    for o in offsets.iter().copied() {
        while let Some(change) = changes_iter.next_if(|c| c.old_pos() < o.into()) {
            mdat_shift += change.len_diff();
        }

        o.shift(mdat_shift)?.write(writer)?;
    }
    Ok(())
}

macro_rules! write_or_ignore {
    (nowrite, $($write:tt)*) => {
        Ok(())
    };
    (, $($write:tt)*) => {
        $($write)*
    };
}

// false positive
#[allow(unused)]
macro_rules! test_or_ignore {
    (nowrite, $($write:tt)*) => {};
    (, $($write:tt)*) => {
        $($write)*
    };
}

macro_rules! atom_ref {
    ($($name:ident $(<$lifetime:lifetime>)? $($nowrite:ident)? ,)+) => {
        #[derive(Debug)]
        pub enum AtomRef<'a> {
            $($name(&'a $name $(<$lifetime>)?)),+
        }

        impl AtomRef<'_> {
            pub fn write(&self, writer: &mut impl Write, changes: &[Change<'_>]) -> crate::Result<()> {
                match self {
                    #[allow(unused)]
                    $(Self::$name(a) => write_or_ignore!($($nowrite)?, {a.write(writer, changes)}),)+
                }
            }

            pub fn fourcc(&self) -> Fourcc {
                match self {
                    $(Self::$name(_) => $name::FOURCC,)+
                }
            }

            fn len(&self) -> u64 {
                match self {
                    $(Self::$name(a) => a.len(),)+
                }
            }
        }

        #[cfg(test)]
        mod verify_written_length {
            use super::*;

            $(
                test_or_ignore! { $($nowrite)?,
                    #[test]
                    #[allow(non_snake_case)]
                    fn $name() {
                        let atom = $name::default();
                        let changes = [];

                        let mut buf: Vec<u8> = Vec::new();
                        let mut cursor = std::io::Cursor::new(&mut buf);
                        atom.write(&mut cursor, &changes).unwrap();

                        let buf_size = buf.len() as u64;

                        let mut cursor = std::io::Cursor::new(&buf);
                        cursor.seek(SeekFrom::Start(0)).unwrap();
                        let head = head::parse(&mut cursor, buf_size).unwrap();

                        assert_eq!(atom.len(), head.len());
                        assert_eq!(atom.len(), buf_size);
                    }
                }
            )+
        }
    };
}

atom_ref!(
    Moov<'a> nowrite,
    Udta<'a>,
    Chpl<'a>,
    Meta<'a>,
    Hdlr,
    Ilst<'a>,
    Trak,
    Tref,
    Chap,
    Mdia,
    Minf,
    Dinf,
    Dref,
    Url,
    Gmhd,
    Gmin,
    Text,
    Stbl,
    Stsd,
    Stts,
    Stsc,
    Stsz,
    Stco,
    Co64,
);

#[cfg(test)]
mod tests {
    use super::*;

    fn bounds(pos: u64, len: u64) -> AtomBounds {
        head::test_bounds(pos, Size::from(len - Head::NORMAL_SIZE))
    }

    fn remove(pos: u64, len: u64) -> Change<'static> {
        Change::Remove(RemoveAtom {
            bounds: Box::leak(Box::new(bounds(pos, len))),
            level: 1,
        })
    }

    fn insert(pos: u64, content_len: u64) -> Change<'static> {
        let atom = Box::leak(Box::new(Hdlr {
            state: State::Insert,
            data: Cow::Owned(vec![0; content_len as usize]),
        }));
        Change::Insert(InsertAtom { pos, atom: AtomRef::Hdlr(atom), level: 1 })
    }

    fn shifted<T: ChunkOffsetInt + FromU64>(offsets: &[T], changes: &[Change<'_>]) -> Vec<T> {
        let mut buf = Vec::new();
        write_shifted_offsets(&mut std::io::Cursor::new(&mut buf), offsets, changes).unwrap();
        let entry_size = (buf.len() / offsets.len()) as u64;
        let mut shifted = Vec::with_capacity(offsets.len());
        let mut reader = std::io::Cursor::new(&buf);
        for _ in 0..offsets.len() {
            let offset = if entry_size == 4 {
                T::from_u64(reader.read_be_u32().unwrap().into())
            } else {
                T::from_u64(reader.read_be_u64().unwrap())
            };
            shifted.push(offset);
        }
        shifted
    }

    trait FromU64 {
        fn from_u64(value: u64) -> Self;
    }
    impl FromU64 for u32 {
        fn from_u64(value: u64) -> Self {
            value as Self
        }
    }
    impl FromU64 for u64 {
        fn from_u64(value: u64) -> Self {
            value
        }
    }

    #[test]
    fn shift_is_not_applied_to_chunks_before_the_change() {
        let changes = [remove(60, 40)];

        assert_eq!(shifted(&[50u32, 150, 250], &changes), [50, 110, 210]);
        assert_eq!(shifted(&[50u64, 150, 250], &changes), [50, 110, 210]);
    }

    #[test]
    fn shift_accumulates_across_multiple_length_changes() {
        let changes = [
            remove(60, 40),              // -40
            insert(200, 17),             // +25 (8 byte atom head plus 17 bytes content)
            Change::RemoveMdat(400, 15), // -15
        ];

        let offsets_32 = [50u32, 150, 350, 600];
        assert_eq!(shifted(&offsets_32, &changes), [50, 110, 335, 570]);

        let offsets_64 = [50u64, 150, 350, 600];
        assert_eq!(shifted(&offsets_64, &changes), [50, 110, 335, 570]);
    }

    #[test]
    fn growing_preceding_data_shifts_offsets_on_both_sides_of_four_gib() {
        let four_gib = u32::MAX as u64 + 1;
        let changes = [insert(500, 1017)]; // total atom length of 1025
        let offsets = [1000u64, four_gib - 10, four_gib, four_gib + 10, four_gib * 2];
        let expected =
            [2025, four_gib - 10 + 1025, four_gib + 1025, four_gib + 1035, four_gib * 2 + 1025];

        assert_eq!(shifted(&offsets, &changes), expected);
    }

    #[test]
    fn shrinking_preceding_data_shifts_offsets_on_both_sides_of_four_gib() {
        let four_gib = u32::MAX as u64 + 1;
        let changes = [remove(500, 1025)]; // removed atom is 1025 bytes long
        let offsets = [2000u64, four_gib - 10, four_gib, four_gib + 10, four_gib * 2];
        let expected =
            [975u64, four_gib - 10 - 1025, four_gib - 1025, four_gib - 1015, four_gib * 2 - 1025];

        assert_eq!(shifted(&offsets, &changes), expected);
    }

    #[test]
    fn u32_overflow_and_underflow_are_reported() {
        let grow = [insert(100, 100)];
        let offsets = [u32::MAX - 10, u32::MAX];
        let mut buf = Vec::new();
        assert!(
            write_shifted_offsets(&mut std::io::Cursor::new(&mut buf), &offsets, &grow).is_err()
        );

        let shrink = [remove(0, 100)];
        let offsets = [50u32];
        let mut buf = Vec::new();
        assert!(
            write_shifted_offsets(&mut std::io::Cursor::new(&mut buf), &offsets, &shrink).is_err()
        );
    }

    #[test]
    fn u64_overflow_and_underflow_are_reported() {
        let grow = [insert(0, 1)];
        let offsets = [i64::MAX as u64];
        let mut buf = Vec::new();
        assert!(
            write_shifted_offsets(&mut std::io::Cursor::new(&mut buf), &offsets, &grow).is_err()
        );

        let shrink = [remove(0, 100)];
        let offsets = [50u64];
        let mut buf = Vec::new();
        assert!(
            write_shifted_offsets(&mut std::io::Cursor::new(&mut buf), &offsets, &shrink).is_err()
        );
    }
}
