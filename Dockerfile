FROM alpine:3.18
RUN apk add --no-cache g++ make cmake git
WORKDIR /app
COPY . .
# 拉取依赖
RUN git clone https://github.com/yhirose/cpp-httplib.git && \
    git clone https://github.com/nlohmann/json.git
# 编译
RUN g++ -std=c++17 -I./cpp-httplib -I./json/include server.cpp -pthread -o server
EXPOSE 8080
CMD ["./server"]
