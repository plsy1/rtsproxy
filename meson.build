project(
    'rtsproxy',
    'cpp',
    default_options: ['cpp_std=c++17','warning_level=2','prefix=' + meson.current_source_dir()],
)

is_static = get_option('isstatic')

ldflags = []

if is_static
    message('Static compilation is enabled.')
    ldflags += ['-static']
else
    message('Static compilation is disabled.')
endif

cpp = meson.get_compiler('cpp')
atomic_dep = cpp.find_library('atomic', required: false)

src_dir = 'src'

inc_dir = 'include'

executable(
    'rtsproxy',
    files(
        src_dir / 'main.cpp',
        src_dir / 'epoll_loop.cpp',
        src_dir / 'rtsp_client.cpp',
        src_dir / 'rtsp_mitm_client.cpp',
        src_dir / 'rtsp_parser.cpp',
        src_dir / 'stun_client.cpp',
        src_dir / 'http_handle.cpp',
        src_dir / 'rtsp_handle.cpp',
        src_dir / 'logger.cpp',
        src_dir / 'server_config.cpp',
        src_dir / 'http_parser.cpp',
        src_dir / 'socket_helper.cpp',
        src_dir / 'blacklist_checker.cpp',
        src_dir / 'dns_resolver.cpp',
        src_dir / 'port_pool.cpp',
        src_dir / 'rtp_pipeline.cpp',
    ),
    include_directories: include_directories(inc_dir),
    install: true,
    install_dir: 'bin',
    link_args: ldflags,
    dependencies: [atomic_dep],
)