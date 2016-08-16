$(shell chmod u+x build_tools)
cross :
	./build_tools -a
local :
	./build_tools -b
clean :
	./build_tools -c
