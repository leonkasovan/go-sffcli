# sffcli:  
CLI Tool to extract sprites (into PNG format) and palettes (into ACT format) from Mugen SFF (both v1 and v2)  
SFF v1 supports pcx.  
SFF v2 supports rle5, rle8, lz5, png.  

## Usage
```
sffcli
sffcli -pal
sffcli -pal [char1.sff] [char2.sff] ...

When called with no args it will read all sff files in current directory
Options:
  -pal : save palettes 
```

## Output
```
PNG Format: charname group_id number.png
ACT Format: charname group_id number.act

Example:
kfmZ 800 6.png
kfmZ 800 7.png
kfmZ 800 8.png
kfmZ 800 9.png
kfmZ 9000 0.png
kfmZ 9000 1.png

kfmZ 1 1.act
kfmZ 1 2.act
kfmZ 1 3.act
kfmZ 1 4.act
kfmZ 1 5.act
kfmZ 9000 1.act
```

## Build
```
git clone https://github.com/leonkasovan/go-sffcli.git
make
```

## Dependencies
`none`
Just run it.  

## Install
Download executable from [here](https://github.com/leonkasovan/go-sffcli/releases/download/1.0/sffcli.zip), extract and run it.  

## Todo:
- create 1 big png image atlas from all sprite
- convert to DDS (DirectDraw Surface) format
- output to specific directory (done)
- customize filename format