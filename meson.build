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
        # Core
        src_dir / 'core/epoll_loop.cpp',
        src_dir / 'core/logger.cpp',
        src_dir / 'core/server_config.cpp',
        src_dir / 'core/proxy_server.cpp',
        src_dir / 'core/port_pool.cpp',
        # Handlers
        src_dir / 'handlers/master_handle.cpp',
        src_dir / 'handlers/api_handle.cpp',
        src_dir / 'handlers/rtsp_to_http_handle.cpp',
        src_dir / 'handlers/rtsp_to_rtsp_handle.cpp',
        # Clients
        src_dir / 'clients/rtsp_to_http_client.cpp',
        src_dir / 'clients/rtsp_to_rtsp_client.cpp',
        # Protocol
        src_dir / 'protocol/request_parser.cpp',
        src_dir / 'protocol/rtsp_parser.cpp',
        src_dir / 'protocol/rtp_pipeline.cpp',
        # Utils
        src_dir / 'utils/socket_helper.cpp',
        src_dir / 'utils/blacklist_checker.cpp',
        src_dir / 'utils/dns_resolver.cpp',
        src_dir / 'utils/url_rewriter.cpp',
        src_dir / 'utils/stun_client.cpp',
    ),
    include_directories: include_directories(inc_dir),
    install: true,
    install_dir: 'bin',
    link_args: ldflags,
    dependencies: [atomic_dep],
)