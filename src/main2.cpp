#include <boost/asio.hpp>
#include <exception>
#include <iostream>
#include <memory>

constexpr int port = 1234;
using boost::asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(tcp::socket socket) : socket_(std::move(socket)) {}
  void start() { do_read(); }

 private:
  static const size_t max_length = 1024;
  void do_read();
  void do_write(std::size_t length);
  tcp::socket socket_;
  char data_[max_length];
};

class EchoServer {
 public:
  EchoServer(boost::asio::io_context& ctx, short port)
      : m_acceptor(ctx, tcp::endpoint(tcp::v4(), port)) {
    do_accept();
  }

 private:
  void do_accept() {
    m_acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
      if (not ec) {
        std::make_shared<Session>(std::move(socket))->start();
      }
      do_accept();
    });
  }
  tcp::acceptor m_acceptor;
};

int main() {
  try {
    boost::asio::io_context io_context;
    EchoServer server(io_context, port);
    io_context.run();
  } catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << std::endl;
  }
  return 0;
}
void Session::do_read() {
  auto self(shared_from_this());
  socket_.async_read_some(boost::asio::buffer(data_, max_length),
                          [this, self](boost::system::error_code ec, std::size_t length) {
                            if (!ec) {
                              do_write(length);
                            }
                          });
}

void Session::do_write(std::size_t length) {
  auto self(shared_from_this());
  boost::asio::async_write(socket_, boost::asio::buffer(data_, length),
                           [this, self](boost::system::error_code ec, std::size_t length) {
                             if (!ec) {
                               do_read();
                             }
                           });
}
