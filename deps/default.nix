# Nixpkgs dependencies shared by every package and every dev shell.
# Add a package here once. packageFor and shellFor both apply the whole set.
{ pkgs }:
{
  boost = pkgs.boost190;
  spdlog = pkgs.spdlog;
  openssl = pkgs.openssl;
  libnats-c = pkgs.libnats-c;
}
