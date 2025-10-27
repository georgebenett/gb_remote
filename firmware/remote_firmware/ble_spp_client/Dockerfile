FROM espressif/idf:v5.3

# Set working directory
WORKDIR /project

# Install additional dependencies if needed
RUN apt-get update && apt-get install -y \
    git \
    cmake \
    && rm -rf /var/lib/apt/lists/*

# Copy the project files
COPY CMakeLists.txt .
COPY main ./main
COPY sdkconfig.defaults .
COPY partitions.csv .
COPY managed_components ./managed_components
COPY sdkconfig.defaults.esp32c3 .
COPY sdkconfig.defaults.esp32s3 .
COPY build.sh .

# Default command
#CMD ["/project/build.sh"]

# clear && docker build -f Dockerfile -t gb_remote . && docker run -it --rm gb_remote