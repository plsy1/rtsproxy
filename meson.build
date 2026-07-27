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

inc = include_directories(inc_dir)

# Modules with no epoll/socket dependency. They form a self-contained link set,
# which is what makes them unit-testable on their own.
testable_srcs = files(
    src_dir / 'core/logger.cpp',
    src_dir / 'core/port_pool.cpp',
    src_dir / 'core/server_config.cpp',
    src_dir / 'protocol/request_parser.cpp',
    src_dir / 'protocol/rtsp_parser.cpp',
    src_dir / 'protocol/rtp_pipeline.cpp',
    src_dir / 'utils/blacklist_checker.cpp',
    src_dir / 'utils/dns_resolver.cpp',
    src_dir / 'utils/url_rewriter.cpp',
    src_dir / 'utils/stun_client.cpp',
)

# Everything that drives the event loop and the sockets.
app_srcs = files(
    src_dir / 'main.cpp',
    src_dir / 'core/epoll_loop.cpp',
    src_dir / 'core/proxy_server.cpp',
    src_dir / 'handlers/master_handle.cpp',
    src_dir / 'handlers/api_handle.cpp',
    src_dir / 'handlers/rtsp_to_http_handle.cpp',
    src_dir / 'handlers/rtsp_to_rtsp_handle.cpp',
    src_dir / 'clients/rtsp_to_http_client.cpp',
    src_dir / 'clients/rtsp_to_rtsp_client.cpp',
    src_dir / 'utils/socket_helper.cpp',
)

testable_lib = static_library(
    'rtsproxy_testable',
    testable_srcs,
    include_directories: inc,
    install: false,
)

executable(
    'rtsproxy',
    app_srcs,
    include_directories: inc,
    link_with: testable_lib,
    install: true,
    install_dir: 'bin',
    link_args: ldflags,
    dependencies: [atomic_dep],
)

# Unit tests are opt-in so that release and OpenWrt SDK builds, which never copy
# the test/ directory, are unaffected.
if get_option('tests')
    test_names = [
        'url_rewriter',
        'blacklist_checker',
        'rtsp_parser',
        'request_parser',
        'rtp_pipeline',
        'port_pool',
        'server_config',
        'stun_client',
    ]

    foreach name : test_names
        test_exe = executable(
            'test_' + name,
            'test' / 'unit' / ('test_' + name + '.cpp'),
            include_directories: [inc, include_directories('test/unit')],
            link_with: testable_lib,
            install: false,
            dependencies: [atomic_dep],
        )
        test(name, test_exe, protocol: 'exitcode')
    endforeach

    test(
        'install_logic',
        find_program('sh'),
        args: [meson.current_source_dir() / 'test' / 'test_install_logic.sh'],
        protocol: 'exitcode',
    )
endif
