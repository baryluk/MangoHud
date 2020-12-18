rpc_common_sources = files('common.c')

protoc = find_program('protoc')
protoc_gen_nanopb = find_program('protoc-gen-nanopb')

gen_pb_py = meson.current_source_dir() + '/gen-pb.py'

mangohud_pb = custom_target('gen-pb',
	input   : ['mangohud.proto', 'mangohud.options'],
	output  : ['mangohud.pb.c', 'mangohud.pb.h'],
	command : [gen_pb_py, protoc.path(), protoc_gen_nanopb.path(), '@INPUT0@', '@INPUT1@', meson.current_build_dir()])

rpc_common_sources += mangohud_pb

mangohud_pb_headers = mangohud_pb[1]

rpc_common = static_library(
  'common',
  rpc_common_sources,
  mangohud_pb,
  c_args : [
    pre_args,
  ],
  cpp_args : [
    pre_args,
  ],
  dependencies : [
    nanopb_dep,
  ],
)

executable('mangohud_server', ['server.cpp', mangohud_pb_headers],  dependencies : [nanopb_dep], link_with : rpc_common, install : true)
executable('mangohud_client_demo', ['client_demo.cpp', mangohud_pb_headers], dependencies : [nanopb_dep], link_with : rpc_common, install : false)
