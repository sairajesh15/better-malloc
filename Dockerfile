# Dockerfile for Project 23: A 'Better' Malloc (Linux x86_64 / ARM64 Live C Bridge)
# Note: Linux container deployment is platform-guarded. Marked as 'untested' on Linux host if built in macOS environment.

FROM ubuntu:22.04

# Install build toolchain and Python runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc \
    make \
    python3 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy repository source files
COPY . /app

# Build native C shared libraries and test suites
RUN make clean && make all

# Expose web bridge port
EXPOSE 8080

# Environment variables
ENV PORT=8080
ENV PYTHONUNBUFFERED=1

# Launch the live C bridge web server
CMD ["python3", "server.py"]
