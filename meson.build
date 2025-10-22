project(
    'rtsproxy',
    'cpp',
    default_options: ['cpp_std=c++17','warning_level=2','prefix=' + meson.current_source_dir()],
)

src_dir = 'src'

ldflags = ['-static']

executable(
    'rtsproxy',
    files(
        src_dir / 'main.cpp',
        src_dir / 'epoll.cpp',
        src_dir / 'rtsp.cpp',
        src_dir / 'stun.cpp',
        src_dir / 'handle_http.cpp',
        src_dir / 'logs.cpp',
        src_dir / 'config.cpp',
    ),
    include_directories: include_directories(src_dir),
    install: true,
    install_dir: 'bin',
    cpp_args: ['-O2', '-Wall'],
    link_args: ldflags,
)