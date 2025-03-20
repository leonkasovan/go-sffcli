/* 
 SFF CLI tool to extract sprites (into PNG format) and palettes (into ACT format) from SFF files
 Usage: sffcli.exe <sff_file>
 Example: sffcli.exe chars.sff
 Build windows: go build -trimpath -ldflags="-s -w" -o sffcli.exe .\src\
 Build linux: go build -trimpath -ldflags="-s -w" -o sffcli src/main.go
*/

package main

/*
// Windows Build Tags
#cgo windows CFLAGS: -D_WIN32
#cgo windows LDFLAGS: -lgdi32

// Linux Build Tags
#cgo linux CFLAGS: -D__linux -D__linux__
#cgo linux LDFLAGS: -lm

#include "pack.c"
*/
import "C"
import (
	"bytes"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"strings"
	"unsafe"

	"github.com/leonkasovan/sffcli/packages/physfs"
)

const MaxPalNo = 32

type Texture interface {
	Dummy() bool
}

func Min(arg ...int32) (min int32) {
	for i, x := range arg {
		if i == 0 || x < min {
			min = x
		}
	}
	return
}

func Max(arg ...int32) (max int32) {
	for i, x := range arg {
		if i == 0 || x > max {
			max = x
		}
	}
	return
}
func Clamp(x, a, b int32) int32 {
	return Max(a, Min(x, b))
}

type PaletteList struct {
	palettes   [][]uint32
	paletteMap []int
	PalTable   map[[2]int16]int
	numcols    map[[2]int16]int
	PalTex     []Texture
}

func (pl *PaletteList) init() {
	pl.palettes = nil
	pl.paletteMap = nil
	pl.PalTable = make(map[[2]int16]int)
	pl.numcols = make(map[[2]int16]int)
	pl.PalTex = nil
}

func (pl *PaletteList) SetSource(i int, p []uint32) {
	if i < len(pl.paletteMap) {
		pl.paletteMap[i] = i
	} else {
		for i > len(pl.paletteMap) {
			pl.paletteMap = append(pl.paletteMap, len(pl.paletteMap))
		}
		pl.paletteMap = append(pl.paletteMap, i)
	}
	if i < len(pl.palettes) {
		pl.palettes[i] = p
	} else {
		for i > len(pl.palettes) {
			pl.palettes = append(pl.palettes, nil)
		}
		pl.palettes = append(pl.palettes, p)
		pl.PalTex = append(pl.PalTex, nil)
	}
}
func (pl *PaletteList) NewPal() (i int, p []uint32) {
	i, p = len(pl.palettes), make([]uint32, 256)
	pl.SetSource(i, p)
	return
}
func (pl *PaletteList) Get(i int) []uint32 {
	return pl.palettes[pl.paletteMap[i]]
}
func (pl *PaletteList) Remap(source int, destination int) {
	pl.paletteMap[source] = destination
}
func (pl *PaletteList) ResetRemap() {
	for i := range pl.paletteMap {
		pl.paletteMap[i] = i
	}
}
func (pl *PaletteList) GetPalMap() []int {
	pm := make([]int, len(pl.paletteMap))
	copy(pm, pl.paletteMap)
	return pm
}

func (pl *PaletteList) SwapPalMap(palMap *[]int) bool {
	if len(*palMap) != len(pl.paletteMap) {
		return false
	}
	*palMap, pl.paletteMap = pl.paletteMap, *palMap
	return true
}

type SffHeader struct {
	Ver0, Ver1, Ver2, Ver3   byte
	FirstSpriteHeaderOffset  uint32
	FirstPaletteHeaderOffset uint32
	NumberOfSprites          uint32
	NumberOfPalettes         uint32
}

func (sh *SffHeader) Read(r io.Reader, lofs *uint32, tofs *uint32) error {
	buf := make([]byte, 12)
	n, err := r.Read(buf)
	if err != nil {
		return err
	}
	if string(buf[:n]) != "ElecbyteSpr\x00" {
		return fmt.Errorf("Unrecognized SFF file, invalid header")
	}
	read := func(x interface{}) error {
		return binary.Read(r, binary.LittleEndian, x)
	}
	if err := read(&sh.Ver3); err != nil {
		return err
	}
	if err := read(&sh.Ver2); err != nil {
		return err
	}
	if err := read(&sh.Ver1); err != nil {
		return err
	}
	if err := read(&sh.Ver0); err != nil {
		return err
	}
	var dummy uint32
	if err := read(&dummy); err != nil {
		return err
	}
	switch sh.Ver0 {
	case 1:
		sh.FirstPaletteHeaderOffset, sh.NumberOfPalettes = 0, 0
		if err := read(&sh.NumberOfSprites); err != nil {
			return err
		}
		if err := read(&sh.FirstSpriteHeaderOffset); err != nil {
			return err
		}
		if err := read(&dummy); err != nil {
			return err
		}
	case 2:
		for i := 0; i < 4; i++ {
			if err := read(&dummy); err != nil {
				return err
			}
		}
		if err := read(&sh.FirstSpriteHeaderOffset); err != nil {
			return err
		}
		if err := read(&sh.NumberOfSprites); err != nil {
			return err
		}
		if err := read(&sh.FirstPaletteHeaderOffset); err != nil {
			return err
		}
		if err := read(&sh.NumberOfPalettes); err != nil {
			return err
		}
		if err := read(lofs); err != nil {
			return err
		}
		if err := read(&dummy); err != nil {
			return err
		}
		if err := read(tofs); err != nil {
			return err
		}
	default:
		return fmt.Errorf("Unrecognized SFF version")
	}
	return nil
}

type Sprite struct {
	Pal      []uint32
	Tex      Texture
	Group    int16 // References above 32767 will be read as negative. This is true to SFF format however
	Number   int16
	Size     [2]uint16
	Offset   [2]int16
	palidx   int
	rle      int
	coldepth byte
	paltemp  []uint32
	PalTex   Texture
}

func newSprite() *Sprite {
	return &Sprite{palidx: -1}
}

func (s *Sprite) shareCopy(src *Sprite) {
	s.Pal = src.Pal
	s.Tex = src.Tex
	s.Size = src.Size
	if s.palidx < 0 {
		s.palidx = src.palidx
	}
	s.coldepth = src.coldepth
	//s.paltemp = src.paltemp
	//s.PalTex = src.PalTex
}
func (s *Sprite) GetPal(pl *PaletteList) []uint32 {
	if len(s.Pal) > 0 || s.coldepth > 8 {
		return s.Pal
	}
	return pl.Get(int(s.palidx)) //pl.palettes[pl.paletteMap[int(s.palidx)]]
}

func (s *Sprite) GetPalTex(pl *PaletteList) Texture {
	if s.coldepth > 8 {
		return nil
	}
	return pl.PalTex[pl.paletteMap[int(s.palidx)]]
}

func (s *Sprite) readHeader(r io.Reader, ofs, size *uint32, link *uint16) error {
	read := func(x interface{}) error {
		return binary.Read(r, binary.LittleEndian, x)
	}
	if err := read(ofs); err != nil {
		return err
	}
	if err := read(size); err != nil {
		return err
	}
	if err := read(s.Offset[:]); err != nil {
		return err
	}
	if err := read(&s.Group); err != nil {
		return err
	}
	if err := read(&s.Number); err != nil {
		return err
	}
	if err := read(link); err != nil {
		return err
	}
	return nil
}

func (s *Sprite) readPcxHeader(f *physfs.File, offset int64) error {
	f.Seek(offset, 0)
	read := func(x interface{}) error {
		return binary.Read(f, binary.LittleEndian, x)
	}
	var dummy uint16
	if err := read(&dummy); err != nil {
		return err
	}
	var encoding, bpp byte
	if err := read(&encoding); err != nil {
		return err
	}
	if err := read(&bpp); err != nil {
		return err
	}
	if bpp != 8 {
		return fmt.Errorf(fmt.Sprintf("Invalid PCX color depth: expected 8-bit, got %v", bpp))
	}
	var rect [4]uint16
	if err := read(rect[:]); err != nil {
		return err
	}
	f.Seek(offset+66, 0)
	var bpl uint16
	if err := read(&bpl); err != nil {
		return err
	}
	s.Size[0] = rect[2] - rect[0] + 1
	s.Size[1] = rect[3] - rect[1] + 1
	if encoding == 1 {
		s.rle = int(bpl)
	} else {
		s.rle = 0
	}
	return nil
}
func (s *Sprite) RlePcxDecode(rle []byte) (p []byte) {
	if len(rle) == 0 || s.rle <= 0 {
		return rle
	}
	p = make([]byte, int(s.Size[0])*int(s.Size[1]))
	i, j, k, w := 0, 0, 0, int(s.Size[0])
	for j < len(p) {
		n, d := 1, rle[i]
		if i < len(rle)-1 {
			i++
		}
		if d >= 0xc0 {
			n = int(d & 0x3f)
			d = rle[i]
			if i < len(rle)-1 {
				i++
			}
		}
		for ; n > 0; n-- {
			if k < w && j < len(p) {
				p[j] = d
				j++
			}
			k++
			if k == s.rle {
				k = 0
				n = 1
			}
		}
	}
	s.rle = 0
	return
}
func (s *Sprite) read(f *physfs.File, sff *Sff, offset int64, datasize uint32,
	nextSubheader uint32, prev *Sprite, pl *PaletteList, c00 bool) error {
	if int64(nextSubheader) > offset {
		// Ignore datasize except last
		datasize = nextSubheader - uint32(offset)
	}
	read := func(x interface{}) error {
		return binary.Read(f, binary.LittleEndian, x)
	}
	var ps byte
	if err := read(&ps); err != nil {
		return err
	}
	paletteSame := ps != 0 && prev != nil
	if err := s.readPcxHeader(f, offset); err != nil {
		return err
	}
	f.Seek(offset+128, 0)
	var palSize uint32
	if c00 || paletteSame {
		palSize = 0
	} else {
		palSize = 768
	}
	if datasize < 128+palSize {
		datasize = 128 + palSize
	}
	px := make([]byte, datasize-(128+palSize))
	if err := read(px); err != nil {
		return err
	}
	if paletteSame {
		if prev != nil {
			s.palidx = prev.palidx
		}
		if s.palidx < 0 {
			s.palidx, _ = pl.NewPal()
		}
	} else {
		var pal []uint32
		s.palidx, pal = pl.NewPal()
		if c00 {
			f.Seek(offset+int64(datasize)-768, 0)
		}
		var rgb [3]byte
		for i := range pal {
			if err := read(rgb[:]); err != nil {
				return err
			}
			var alpha byte = 255
			if i == 0 {
				alpha = 0
			}
			pal[i] = uint32(alpha)<<24 | uint32(rgb[2])<<16 | uint32(rgb[1])<<8 | uint32(rgb[0])
		}
		savePalette(pal, fmt.Sprintf("%v %v %v.act", "char_pal", s.Group, s.Number))
	}

	// Create a new Paletted image
	img := image.NewPaletted(image.Rect(0, 0, int(s.Size[0]), int(s.Size[1])), genPalette(pl.Get(s.palidx)))
	img.Pix = s.RlePcxDecode(px)

	// Extract filename without extension
	baseFilename := strings.TrimSuffix(sff.filename, filepath.Ext(sff.filename))
	pngFilename := fmt.Sprintf("%v %v %v.png", s.Group, s.Number, baseFilename)
	// fmt.Printf("Saving %v with Palette id=%v\n", pngFilename, s.palidx)

	// Save the image to a file
	fo, err := os.Create(pngFilename)
	if err != nil {
		return fmt.Errorf("Error creating file %v: %v", pngFilename, err)
	}
	defer fo.Close()

	return png.Encode(fo, img)
}

func (s *Sprite) readHeaderV2(r io.Reader, ofs *uint32, size *uint32,
	lofs uint32, tofs uint32, link *uint16) error {
	read := func(x interface{}) error {
		return binary.Read(r, binary.LittleEndian, x)
	}
	if err := read(&s.Group); err != nil {
		return err
	}
	if err := read(&s.Number); err != nil {
		return err
	}
	if err := read(s.Size[:]); err != nil {
		return err
	}
	if err := read(s.Offset[:]); err != nil {
		return err
	}
	if err := read(link); err != nil {
		return err
	}
	var format byte
	if err := read(&format); err != nil {
		return err
	}
	s.rle = -int(format)
	if err := read(&s.coldepth); err != nil {
		return err
	}
	if err := read(ofs); err != nil {
		return err
	}
	if err := read(size); err != nil {
		return err
	}
	var tmp uint16
	if err := read(&tmp); err != nil {
		return err
	}
	s.palidx = int(tmp)
	if err := read(&tmp); err != nil {
		return err
	}
	if tmp&1 == 0 {
		*ofs += lofs
	} else {
		*ofs += tofs
	}
	return nil
}
func (s *Sprite) Rle8Decode(rle []byte) (p []byte) {
	if len(rle) == 0 {
		return rle
	}
	p = make([]byte, int(s.Size[0])*int(s.Size[1]))
	i, j := 0, 0
	for j < len(p) {
		n, d := 1, rle[i]
		if i < len(rle)-1 {
			i++
		}
		if d&0xc0 == 0x40 {
			n = int(d & 0x3f)
			d = rle[i]
			if i < len(rle)-1 {
				i++
			}
		}
		for ; n > 0; n-- {
			if j < len(p) {
				p[j] = d
				j++
			}
		}
	}
	return
}
func (s *Sprite) Rle5Decode(rle []byte) (p []byte) {
	if len(rle) == 0 {
		return rle
	}
	p = make([]byte, int(s.Size[0])*int(s.Size[1]))
	i, j := 0, 0
	for j < len(p) {
		rl := int(rle[i])
		if i < len(rle)-1 {
			i++
		}
		dl := int(rle[i] & 0x7f)
		c := byte(0)
		if rle[i]>>7 != 0 {
			if i < len(rle)-1 {
				i++
			}
			c = rle[i]
		}
		if i < len(rle)-1 {
			i++
		}
		for {
			if j < len(p) {
				p[j] = c
				j++
			}
			rl--
			if rl < 0 {
				dl--
				if dl < 0 {
					break
				}
				c = rle[i] & 0x1f
				rl = int(rle[i] >> 5)
				if i < len(rle)-1 {
					i++
				}
			}
		}
	}
	return
}
func (s *Sprite) Lz5Decode(rle []byte) (p []byte) {
	if len(rle) == 0 {
		return rle
	}
	p = make([]byte, int(s.Size[0])*int(s.Size[1]))
	i, j, n := 0, 0, 0
	ct, cts, rb, rbc := rle[i], uint(0), byte(0), uint(0)
	if i < len(rle)-1 {
		i++
	}
	for j < len(p) {
		d := int(rle[i])
		if i < len(rle)-1 {
			i++
		}
		if ct&byte(1<<cts) != 0 {
			if d&0x3f == 0 {
				d = (d<<2 | int(rle[i])) + 1
				if i < len(rle)-1 {
					i++
				}
				n = int(rle[i]) + 2
				if i < len(rle)-1 {
					i++
				}
			} else {
				rb |= byte(d & 0xc0 >> rbc)
				rbc += 2
				n = int(d & 0x3f)
				if rbc < 8 {
					d = int(rle[i]) + 1
					if i < len(rle)-1 {
						i++
					}
				} else {
					d = int(rb) + 1
					rb, rbc = 0, 0
				}
			}
			for {
				if j < len(p) {
					p[j] = p[j-d]
					j++
				}
				n--
				if n < 0 {
					break
				}
			}
		} else {
			if d&0xe0 == 0 {
				n = int(rle[i]) + 8
				if i < len(rle)-1 {
					i++
				}
			} else {
				n = d >> 5
				d &= 0x1f
			}
			for ; n > 0; n-- {
				if j < len(p) {
					p[j] = byte(d)
					j++
				}
			}
		}
		cts++
		if cts >= 8 {
			ct, cts = rle[i], 0
			if i < len(rle)-1 {
				i++
			}
		}
	}
	return
}

func genPalette(pal []uint32) color.Palette {
	palette := make(color.Palette, len(pal))
	for i, c := range pal {
		palette[i] = color.RGBA{uint8(c), uint8(c >> 8), uint8(c >> 16), uint8(c >> 24)}
	}
	return palette
}

// save palette to file
func savePalette(pal []uint32, filename string) error {
	fo, err := os.Create(filename)
	defer fo.Close()
	if err != nil {
		return fmt.Errorf("Error creating file %v:  %v\n", filename, err)
	} else {
		for _, c := range pal {
			_, err = fo.Write([]byte{uint8(c), uint8(c >> 8), uint8(c >> 16)}) // Write as byte
			if err != nil {
				return fmt.Errorf("Error writing to file: %v\n", err)
			}
		}
		return nil
	}
}

// ReplacePalette replaces the PLTE chunk in a PNG file with a palette from an ACT file.
func replacePalette(pngPath string, actPath string, outputPath string) error {
	// Open ACT palette file (768 bytes)
	actFile, err := os.Open(actPath)
	if err != nil {
		return fmt.Errorf("error opening ACT file: %w", err)
	}
	defer actFile.Close()

	// Read ACT file (768 bytes, 256 colors × 3 bytes each)
	actPalette := make([]byte, 768)
	_, err = actFile.Read(actPalette)
	if err != nil {
		return fmt.Errorf("error reading ACT file: %w", err)
	}

	// Open PNG file
	pngFile, err := os.Open(pngPath)
	if err != nil {
		return fmt.Errorf("error opening PNG file: %w", err)
	}
	defer pngFile.Close()

	// Read PNG signature (8 bytes)
	signature := make([]byte, 8)
	_, err = pngFile.Read(signature)
	if err != nil || !bytes.Equal(signature, []byte{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}) {
		return fmt.Errorf("not a valid PNG file")
	}

	// Buffer to store modified PNG data
	var outputBuffer bytes.Buffer
	outputBuffer.Write(signature) // Write PNG signature

	// Process PNG chunks
	for {
		// Read chunk length (4 bytes)
		lengthBytes := make([]byte, 4)
		_, err := pngFile.Read(lengthBytes)
		if err == io.EOF {
			break // End of file
		} else if err != nil {
			return fmt.Errorf("error reading chunk length: %w", err)
		}
		length := binary.BigEndian.Uint32(lengthBytes)

		// Read chunk type (4 bytes)
		chunkType := make([]byte, 4)
		_, err = pngFile.Read(chunkType)
		if err != nil {
			return fmt.Errorf("error reading chunk type: %w", err)
		}

		// Read chunk data + CRC
		chunkData := make([]byte, length+4) // +4 for CRC
		_, err = pngFile.Read(chunkData)
		if err != nil {
			return fmt.Errorf("error reading chunk data: %w", err)
		}

		// If it's the PLTE chunk, replace it
		if string(chunkType) == "PLTE" {
			fmt.Println("Replacing PLTE chunk with ACT palette...")

			// Trim ACT palette to 256 colors (max PNG palette size)
			if len(actPalette) > 768 {
				actPalette = actPalette[:768]
			}

			// Write new PLTE chunk
			newLength := uint32(len(actPalette))
			binary.Write(&outputBuffer, binary.BigEndian, newLength)
			outputBuffer.Write(chunkType)

			// Write new palette data
			outputBuffer.Write(actPalette)

			// Compute new CRC
			crc := crc32.NewIEEE()
			crc.Write(chunkType)
			crc.Write(actPalette)
			newCRC := crc.Sum32()

			// Write new CRC
			binary.Write(&outputBuffer, binary.BigEndian, newCRC)
		} else {
			// Write the original chunk unchanged
			outputBuffer.Write(lengthBytes)
			outputBuffer.Write(chunkType)
			outputBuffer.Write(chunkData)
		}
	}

	// Save modified PNG
	outputFile, err := os.Create(outputPath)
	if err != nil {
		return fmt.Errorf("error creating output file: %w", err)
	}
	defer outputFile.Close()

	_, err = outputFile.Write(outputBuffer.Bytes())
	if err != nil {
		return fmt.Errorf("error writing modified PNG: %w", err)
	}

	fmt.Println("Palette replaced successfully using ACT file! Saved as:", outputPath)
	return nil
}

func replacePaletteInMemory(imgBuffer *bytes.Buffer, palette []uint32) error {
	// Read PNG signature (8 bytes)
	signature := make([]byte, 8)
	if _, err := imgBuffer.Read(signature); err != nil || !bytes.Equal(signature, []byte{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}) {
		return fmt.Errorf("not a valid PNG file")
	}

	// Buffer to store modified PNG data
	var outputBuffer bytes.Buffer
	outputBuffer.Write(signature) // Write PNG signature

	// Process PNG chunks
	for {
		// Read chunk length (4 bytes)
		lengthBytes := make([]byte, 4)
		if _, err := imgBuffer.Read(lengthBytes); err == io.EOF {
			break // End of file
		} else if err != nil {
			return fmt.Errorf("error reading chunk length: %w", err)
		}
		length := binary.BigEndian.Uint32(lengthBytes)

		// Read chunk type (4 bytes)
		chunkType := make([]byte, 4)
		if _, err := imgBuffer.Read(chunkType); err != nil {
			return fmt.Errorf("error reading chunk type: %w", err)
		}

		// Read chunk data + CRC
		chunkData := make([]byte, length+4) // +4 for CRC
		if _, err := imgBuffer.Read(chunkData); err != nil {
			return fmt.Errorf("error reading chunk data: %w", err)
		}

		// If it's the PLTE chunk, replace it
		if string(chunkType) == "PLTE" {
			// fmt.Println("Replacing PLTE chunk with in-memory palette...")

			// Convert palette to byte slice
			actPalette := make([]byte, 0, 768)
			for _, c := range palette {
				actPalette = append(actPalette, uint8(c), uint8(c>>8), uint8(c>>16))
			}

			// Write new PLTE chunk
			newLength := uint32(len(actPalette))
			binary.Write(&outputBuffer, binary.BigEndian, newLength)
			outputBuffer.Write(chunkType)

			// Write new palette data
			outputBuffer.Write(actPalette)

			// Compute new CRC
			crc := crc32.NewIEEE()
			crc.Write(chunkType)
			crc.Write(actPalette)
			newCRC := crc.Sum32()

			// Write new CRC
			binary.Write(&outputBuffer, binary.BigEndian, newCRC)
		} else {
			// Write the original chunk unchanged
			outputBuffer.Write(lengthBytes)
			outputBuffer.Write(chunkType)
			outputBuffer.Write(chunkData)
		}
	}

	// Replace the contents of imgBuffer with the modified PNG data
	imgBuffer.Reset()
	imgBuffer.Write(outputBuffer.Bytes())

	return nil
}

func saveImageToPNG(sff *Sff, s *Sprite, data []byte) error {
	rect := image.Rect(0, 0, int(s.Size[0]), int(s.Size[1]))

	// Create a new Paletted image
	img := image.NewPaletted(rect, genPalette(sff.palList.Get(s.palidx)))
	img.Pix = data

	// Extract filename without extension
	baseFilename := sff.filename[:len(sff.filename)-4]
	pngFilename := fmt.Sprintf("%v %v %v.png", baseFilename, s.Group, s.Number)
	// fmt.Printf("Saving %v with Palette id=%v\n", pngFilename, s.palidx)

	// Save the image to a file
	fo, err := os.Create(pngFilename)
	if err != nil {
		return fmt.Errorf("Error creating file %v: %v", pngFilename, err)
	}
	defer fo.Close()

	return png.Encode(fo, img)
}

func saveImageToPNG2(sff *Sff, s *Sprite, fi io.Reader, datasize uint32) error {
	// Extract filename without extension
	baseFilename := sff.filename[:len(sff.filename)-4]
	pngFilename := fmt.Sprintf("%v %v %v.png", baseFilename, s.Group, s.Number)
	savePalette(sff.palList.Get(s.palidx), fmt.Sprintf("%v %v %v.act", s.Group, s.Number, baseFilename))
	// fmt.Printf("Saving %v with Palette id=%v\n", pngFilename, s.palidx)

	// Save the image to a file
	fo, err := os.Create(pngFilename)
	if err != nil {
		return fmt.Errorf("Error creating file %v: %v", pngFilename, err)
	}

	// Copy the image data from fi to fo
	_, err = io.CopyN(fo, fi, int64(datasize-4))
	fo.Close()
	if err != nil {
		return fmt.Errorf("Error copying image data: %v", err)
	}

	return replacePalette(pngFilename, fmt.Sprintf("%v %v %v.act", s.Group, s.Number, baseFilename), "fix_"+pngFilename)
}

func saveImageToPNG3(sff *Sff, s *Sprite, fi io.Reader, datasize uint32) error {
	// Extract filename without extension
	baseFilename := sff.filename[:len(sff.filename)-4]
	pngFilename := fmt.Sprintf("%v %v %v.png", baseFilename, s.Group, s.Number)

	// Create an in-memory buffer to store the image data
	var imgBuffer bytes.Buffer

	// Copy the image data from fi to the in-memory buffer
	if _, err := io.CopyN(&imgBuffer, fi, int64(datasize-4)); err != nil {
		return fmt.Errorf("Error copying image data: %v", err)
	}

	// Replace the palette in the PNG data with the palette from memory
	if err := replacePaletteInMemory(&imgBuffer, sff.palList.Get(s.palidx)); err != nil {
		return fmt.Errorf("Error replacing palette: %v", err)
	}

	// Save the modified PNG data to a file
	fo, err := os.Create(pngFilename)
	if err != nil {
		return fmt.Errorf("Error creating file %v: %v", pngFilename, err)
	}
	defer fo.Close()

	if _, err := io.Copy(fo, &imgBuffer); err != nil {
		return fmt.Errorf("Error writing modified PNG: %v", err)
	}

	return nil
}

func (s *Sprite) readV2(f *physfs.File, offset int64, datasize uint32, sff *Sff) error {
	var px []byte
	// var isRaw bool = false

	if s.rle > 0 {
		return nil
	} else if s.rle == 0 {
		f.Seek(offset, 0)
		px = make([]uint8, datasize)
		binary.Read(f, binary.LittleEndian, px)

		switch s.coldepth {
		case 8:
			// Do nothing, px is already in the expected format
		case 24, 32:
			// isRaw = true
		default:
			return fmt.Errorf("Unknown color depth")
		}
	} else {
		f.Seek(offset+4, 0)
		format := -s.rle

		var srcPx []byte
		if 2 <= format && format <= 4 {
			if datasize < 4 {
				datasize = 4
			}
			srcPx = make([]byte, datasize-4)
			if err := binary.Read(f, binary.LittleEndian, srcPx); err != nil {
				return err
			}
		}

		switch format {
			case 2, 3, 4:
				switch format {
				case 2:
					px = s.Rle8Decode(srcPx)
				case 3:
					px = s.Rle5Decode(srcPx)
				case 4:
					px = s.Lz5Decode(srcPx)
				}
				if err := saveImageToPNG(sff, s, px); err != nil {
					return err
				}
				img_tag := C.CString(fmt.Sprintf("%v,%v", s.Group, s.Number))
				C.calculate_image((*C.uchar)(unsafe.Pointer(&px[0])), C.int(s.Size[0]), C.int(s.Size[1]), img_tag)
				defer C.free(unsafe.Pointer(img_tag))
			case 10, 11, 12:
				// fmt.Printf("PNG Format %v. Group:%v Num:%v\n", format, s.Group, s.Number)
				if err := saveImageToPNG3(sff, s, f, datasize); err != nil {
					return err
				}
				C.calculate_image3((*C.FILE)(unsafe.Pointer(f)), C.int(s.Size[0]), C.int(s.Size[1]))
			default:
				return fmt.Errorf("Unknown format")
		}	
	}
	return nil
}

type Sff struct {
	header   SffHeader
	sprites  map[[2]int16]*Sprite
	palList  PaletteList
	filename string
}
type Palette struct {
	palList PaletteList
}

func newSff() (s *Sff) {
	s = &Sff{sprites: make(map[[2]int16]*Sprite)}
	s.palList.init()
	for i := int16(1); i <= int16(MaxPalNo); i++ {
		s.palList.PalTable[[...]int16{1, i}], _ = s.palList.NewPal()
	}
	return
}

func extractSff(filename string, cmdSavePalette bool) (*Sff, error) {
	char := true
	s := newSff()
	s.filename = filename
	f := physfs.OpenRead(filename)
	if f == nil {
		return nil, fmt.Errorf(fmt.Sprintf("File not found: %v", filename))
	}
	defer f.Close()
	var lofs, tofs uint32
	if err := s.header.Read(f, &lofs, &tofs); err != nil {
		return nil, err
	}
	read := func(x interface{}) error {
		return binary.Read(f, binary.LittleEndian, x)
	}
	if s.header.Ver0 != 1 {
		uniquePals := make(map[[2]int16]int)
		for i := 0; i < int(s.header.NumberOfPalettes); i++ {
			f.Seek(int64(s.header.FirstPaletteHeaderOffset)+int64(i*16), 0)
			var gn_ [3]int16
			if err := read(gn_[:]); err != nil {
				return nil, err
			}
			var link uint16
			if err := read(&link); err != nil {
				return nil, err
			}
			var ofs, siz uint32
			if err := read(&ofs); err != nil {
				return nil, err
			}
			if err := read(&siz); err != nil {
				return nil, err
			}
			var pal []uint32
			var idx int
			if old, ok := uniquePals[[...]int16{gn_[0], gn_[1]}]; ok {
				idx = old
				pal = s.palList.Get(old)
				fmt.Printf("%v duplicated palette: %v,%v (%v/%v)\n", filename, gn_[0], gn_[1], i+1, s.header.NumberOfPalettes)
			} else if siz == 0 {
				idx = int(link)
				pal = s.palList.Get(idx)
			} else {
				f.Seek(int64(lofs+ofs), 0)
				pal = make([]uint32, 256)
				var rgba [4]byte
				for i := 0; i < int(siz)/4 && i < len(pal); i++ {
					if err := read(rgba[:]); err != nil {
						return nil, err
					}
					if s.header.Ver2 == 0 {
						if i == 0 {
							rgba[3] = 0
						} else {
							rgba[3] = 255
						}
					}
					pal[i] = uint32(rgba[3])<<24 | uint32(rgba[2])<<16 | uint32(rgba[1])<<8 | uint32(rgba[0])
				}
				if cmdSavePalette {
					savePalette(pal, fmt.Sprintf("%v %v %v.act", filename[:len(filename)-4], gn_[0], gn_[1]))
				}
				idx = i
			}
			uniquePals[[...]int16{gn_[0], gn_[1]}] = idx
			s.palList.SetSource(i, pal)
			s.palList.PalTable[[...]int16{gn_[0], gn_[1]}] = idx
			s.palList.numcols[[...]int16{gn_[0], gn_[1]}] = int(gn_[2])
			if i <= MaxPalNo &&
				s.palList.PalTable[[...]int16{1, int16(i + 1)}] == s.palList.PalTable[[...]int16{gn_[0], gn_[1]}] &&
				gn_[0] != 1 && gn_[1] != int16(i+1) {
				s.palList.PalTable[[...]int16{1, int16(i + 1)}] = -1
			}
			if i <= MaxPalNo && i+1 == int(s.header.NumberOfPalettes) {
				for j := i + 1; j < MaxPalNo; j++ {
					delete(s.palList.PalTable, [...]int16{1, int16(j + 1)}) // Remove extra palette
				}
			}
		}
	}
	spriteList := make([]*Sprite, int(s.header.NumberOfSprites))
	var prev *Sprite
	shofs := int64(s.header.FirstSpriteHeaderOffset)
	for i := 0; i < len(spriteList); i++ {
		f.Seek(shofs, 0)
		spriteList[i] = newSprite()
		var xofs, size uint32
		var indexOfPrevious uint16
		switch s.header.Ver0 {
		case 1:
			if err := spriteList[i].readHeader(f, &xofs, &size,
				&indexOfPrevious); err != nil {
				return nil, err
			}
		case 2:
			if err := spriteList[i].readHeaderV2(f, &xofs, &size,
				lofs, tofs, &indexOfPrevious); err != nil {
				return nil, err
			}
		}
		if size == 0 {
			if int(indexOfPrevious) < i {
				dst, src := spriteList[i], spriteList[int(indexOfPrevious)]
				dst.shareCopy(src)
			} else {
				spriteList[i].palidx = 0 // index out of range
			}
		} else {
			switch s.header.Ver0 {
			case 1:
				if err := spriteList[i].read(f, s, shofs+32, size, xofs, prev, &s.palList, char && (prev == nil || spriteList[i].Group == 0 && spriteList[i].Number == 0)); err != nil {
					return nil, err
				}
			case 2:
				if err := spriteList[i].readV2(f, int64(xofs), size, s); err != nil {
					return nil, err
				}
			}
			prev = spriteList[i]
		}
		if s.sprites[[...]int16{spriteList[i].Group, spriteList[i].Number}] ==
			nil {
			s.sprites[[...]int16{spriteList[i].Group, spriteList[i].Number}] =
				spriteList[i]
		}
		if s.header.Ver0 == 1 {
			shofs = int64(xofs)
		} else {
			shofs += 28
		}
		//~ fmt.Printf("Loading sprite %v/%v: %v,%v %v compressed_size=%v\n", i+1, len(spriteList), spriteList[i].Group, spriteList[i].Number, spriteList[i].Size, size)
	}
	C.print_info()
	return s, nil
}
func (s *Sff) GetSprite(g, n int16) *Sprite {
	if g == -1 {
		return nil
	}
	return s.sprites[[...]int16{g, n}]
}

func main() {
	cmdSavePalette := false
	readAllDirectories := true

	fmt.Printf("sffcli v1.0: tool to extract sprites (into PNG format) and palettes (into ACT format) from Mugen SFF (both v1 and v2)\nCompiled by leonkasovan@gmail.com, 16 Maret 2025\n\n")
	if !physfs.Init(os.Args[0]) {
		fmt.Println("Error: initialize file system")
		return
	}
	defer physfs.Deinit()

	// Mount the current directory
	currentDir, _ := os.Getwd()
	if !physfs.Mount(currentDir, "/", 1) {
		fmt.Printf("Mounting directory \"%v\" [FAIL]\n", currentDir)
	}
	// Set Write Directory
	physfs.SetWriteDir(currentDir)

	if len(os.Args[1:]) > 0 {
		for _, arg := range os.Args[1:] {
			if arg == "-pal" {
				cmdSavePalette = true
			} else if arg == "-h" || arg == "--help" {
				readAllDirectories = false
				fmt.Println("Usage:\n\tsffcli\n\tsffcli -pal\n\tsffcli -pal [char1.sff] [char2.sff] ...\n\nOptions:\n-pal: save palette as ACT file")
			} else {
				sff, err := extractSff(arg, cmdSavePalette)
				if err != nil {
					fmt.Println(err)
				} else {
					readAllDirectories = false
					fmt.Printf("Extract %v (v%d.%d.%d) into %v PNG files", sff.filename, sff.header.Ver0, sff.header.Ver1, sff.header.Ver2, len(sff.sprites))
					if cmdSavePalette {
						fmt.Printf(" and %v ACT files", len(sff.palList.PalTable))
					}
					fmt.Printf("\n")
				}
			}
		}
	}

	if readAllDirectories {
		// Read currentDir directory
		entries, err := physfs.EnumerateFiles("/")
		if err != nil {
			fmt.Printf("failed to read directory %s: %v", currentDir, err)
		}

		// Find sff file and process
		for _, file := range entries {
			if strings.HasSuffix(file, ".sff") {

				sff, err := extractSff(file, cmdSavePalette)
				if err != nil {
					fmt.Println(err)
				} else {
					fmt.Printf("Extract %v (v%d.%d.%d) into %v PNG files", sff.filename, sff.header.Ver0, sff.header.Ver1, sff.header.Ver2, len(sff.sprites))
					if cmdSavePalette {
						fmt.Printf(" and %v ACT files", len(sff.palList.PalTable))
					}
					fmt.Printf("\n")
				}
			}
		}
	}

	// Unmount current directory
	if !physfs.Unmount(currentDir) {
		fmt.Printf("Unmounting directory \"%v\" [FAIL]\n", currentDir)
		return
	}
}
