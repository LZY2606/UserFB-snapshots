# mjpeg

![Build Status](https://github.com/icza/mjpeg/actions/workflows/go.yml/badge.svg)
[![Go Reference](https://pkg.go.dev/badge/github.com/icza/mjpeg.svg)](https://pkg.go.dev/github.com/icza/mjpeg)
[![Go Report Card](https://goreportcard.com/badge/github.com/icza/mjpeg)](https://goreportcard.com/report/github.com/icza/mjpeg)

MJPEG video writer implementation in Go.

## Examples

Let's see an example how to turn the JPEG files `1.jpg`, `2.jpg`, ..., `10.jpg` into a movie file:

    checkErr := func(err error) {
        if err != nil {
            panic(err)
        }
    }

    // Video size: 200x100 pixels, FPS: 2
    aw, err := mjpeg.New("test.avi", 200, 100, 2)
    checkErr(err)

    // Create a movie from images: 1.jpg, 2.jpg, ..., 10.jpg
    for i := 1; i <= 10; i++ {
        data, err := ioutil.ReadFile(fmt.Sprintf("%d.jpg", i))
        checkErr(err)
        checkErr(aw.AddFrame(data))
    }

    checkErr(aw.Close())

Example to add an `image.Image` as a frame to the video:

    aw, err := mjpeg.New("test.avi", 200, 100, 2)
    checkErr(err)

    var img image.Image
    // Acquire / initialize image, e.g.:
    // img = image.NewRGBA(image.Rect(0, 0, 200, 100))

    buf := &bytes.Buffer{}
    checkErr(jpeg.Encode(buf, img, nil))
    checkErr(aw.AddFrame(buf.Bytes()))

    checkErr(aw.Close())

## OpenDML (files beyond 4 GB)

`New` produces a classic single-RIFF AVI file, which stops near 4 GB. For
long recordings use `NewOpenDML`, which writes an OpenDML 1.02 extended
file: a leading `RIFF AVI` segment followed by further `RIFF AVIX`
segments in the same file, tied together by a super index (`indx`),
per-segment standard indexes (`ix00`) and the `odml`/`dmlh` total frame
count. The `AddFrame` / `Close` usage is unchanged:

    aw, err := mjpeg.NewOpenDML("long.avi", 200, 100, 2, mjpeg.OpenDMLOptions{
        SegmentBytes: 1 << 30, // start a new RIFF segment before 1 GiB
        MaxSegments:  256,     // room for 256 segments in the super index
    })
    checkErr(err)

    for i := 1; ; i++ {
        data, err := ioutil.ReadFile(fmt.Sprintf("%d.jpg", i))
        if err != nil {
            break // no more frames
        }
        if err := aw.AddFrame(data); err == mjpeg.ErrTooLarge {
            break // capacity exhausted; the file stays consistent
        }
        checkErr(err)
    }

    checkErr(aw.Close())

`OpenDMLOptions.SegmentBytes` caps the total byte count of each complete
RIFF segment — its 8-byte RIFF shell, headers, padding and final indexes
included. Zero means 1 GiB; a non-zero value must be between 4096 and
1 GiB. `OpenDMLOptions.MaxSegments` is the number of super index entries
reserved in the first segment, and thus the maximum number of segments the
video may consist of; zero means 256, and values up to 65535 are allowed.
The overall capacity of the video is roughly `SegmentBytes * MaxSegments`.

A frame is never split across segments. If the next frame does not fit
into the current segment, the segment is finalized and recording continues
in a new one. `AddFrame` returns `mjpeg.ErrTooLarge` when a frame fits
into no segment at all or the super index is exhausted; the frames
accepted so far remain intact, and `Close` still produces a valid file.
