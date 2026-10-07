# Dockerfile for Development Container.

# Debian 12 (Bookworm) base image.
FROM mcr.microsoft.com/devcontainers/base:bookworm

# Essential dependencies.
COPY ./uni-get-dependencies.sh /tmp/
RUN /tmp/uni-get-dependencies.sh

# Debuggers.
RUN apt-get -y install gdb lldb
