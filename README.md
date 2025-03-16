sffcli:  
tool to extract sprites (into PNG format) and palettes (into ACT format) from Mugen SFF (both v1 and v2)  

## Usage
```
  sffcli
  sffcli -pal
  sffcli -pal [char1.sff] [char2.sff] ...

When called with no args it will read all sff files in current directory
Options:
  -pal : save palettes 
```

## Build
`go build -trimpath -ldflags="-s -w" -o sffcli.exe .\src\`

## Dependencies
`none`
Just run it.  

## Install
Download executable from [here](https://github.com/leonkasovan/go-sffcli/releases/download/1.0/sffcli.zip), extract and run it.  
