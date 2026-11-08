project('expede', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=2'])

# snprintf into fixed buffers truncates on purpose (long titles, names)
add_project_arguments('-DWLR_USE_UNSTABLE', '-Wno-unused-parameter', '-Wno-format-truncation', language: 'c')

wayland_protos = dependency('wayland-protocols')
wayland_scanner = find_program(dependency('wayland-scanner', native: true).get_variable('wayland_scanner'))
wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

protocols = [
	wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	wl_protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
	wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
	wl_protocol_dir / 'unstable/tablet/tablet-unstable-v2.xml',
]
proto_src = []
foreach p : protocols
	proto_src += custom_target(p.underscorify() + '_h', input: p, output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
endforeach

client_protos = []
foreach p : [wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
		wl_protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml']
	client_protos += custom_target(p.underscorify() + '_client_h', input: p, output: '@BASENAME@-client-protocol.h',
		command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
	client_protos += custom_target(p.underscorify() + '_client_c', input: p, output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

wlroots = dependency('wlroots-0.18', required: false)
if not wlroots.found()
	wlroots = dependency('wlroots', version: ['>= 0.18.0', '< 0.19.0'])
endif

executable('expede',
	'src/main.c', 'src/theme.c', 'src/shell.c', 'src/session.c', 'src/apps.c', 'src/volume.c', proto_src,
	dependencies: [
		wlroots,
		dependency('wayland-server'),
		dependency('xkbcommon'),
		dependency('libinput'),
		dependency('cairo'),
		dependency('pangocairo'),
		dependency('gdk-pixbuf-2.0'),
		dependency('libdrm').partial_dependency(compile_args: true, includes: true),
		meson.get_compiler('c').find_library('m'),
	],
	install: true)

executable('expede-settings',
	'src/settings.c', 'src/ui.c', 'src/theme.c', 'src/volume.c', client_protos,
	c_args: ['-DEXPEDE_CLIENT'],
	dependencies: [
		dependency('wayland-client'),
		dependency('xkbcommon'),
		dependency('cairo'),
		dependency('pangocairo'),
		dependency('gdk-pixbuf-2.0'),
	],
	install: true)

executable('expede-explorer',
	'src/explorer.c', 'src/ui.c', 'src/theme.c', client_protos,
	c_args: ['-DEXPEDE_CLIENT'],
	dependencies: [
		dependency('wayland-client'),
		dependency('xkbcommon'),
		dependency('cairo'),
		dependency('pangocairo'),
		dependency('gdk-pixbuf-2.0'),
	],
	install: true)

install_data('data/expede.desktop', install_dir: get_option('datadir') / 'wayland-sessions')
install_data('data/expede-settings.desktop', install_dir: get_option('datadir') / 'applications')
install_data('tools/xprip.py', install_dir: get_option('bindir'), rename: 'expede-xprip', install_mode: 'rwxr-xr-x')
install_data('tools/w7rip.py', install_dir: get_option('bindir'), rename: 'expede-w7rip', install_mode: 'rwxr-xr-x')
install_data('tools/w10rip.py', install_dir: get_option('bindir'), rename: 'expede-w10rip', install_mode: 'rwxr-xr-x')
install_data('data/expede-greeter', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
