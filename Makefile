all: go_release cxx_release cxx_debug

go_release: go_sffcli.exe
cxx_release: sffcli.exe merge_png.exe
cxx_debug: sffcli_debug.exe

go_sffcli.exe: src/main.go
	go build -trimpath -ldflags="-s -w" -o go_sffcli.exe src/main.go

sffcli.exe: src/main.cpp src/libpng/libpng.a
	g++ -O3 -DNDEBUG -o sffcli.exe src/main.cpp src/libpng/libpng.a -lz

merge_png.exe: src/merge_png.cpp src/libpng/libpng.a
	g++ -O3 -DNDEBUG -o merge_png.exe src/merge_png.cpp src/libpng/libpng.a  -lz -fopenmp -std=c++11

sffcli_debug.exe: src/main.cpp
	g++ -fsanitize=address -static-libasan -g -o sffcli_debug.exe src/main.cpp -lpng -lz

src/libpng/libpng.a:
	@make --no-print-directory -s -C src/libpng -f scripts/makefile.gcc libpng.a

clean:
	@rm sffcli.exe sffcli_debug.exe go_sffcli.exe src/libpng/*.a src/libpng/*.o