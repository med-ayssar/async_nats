{ writeShellApplication, nodejs }:
writeShellApplication {
  name = "doc";
  runtimeInputs = [ nodejs ];
  text = ''
    export ASTRO_TELEMETRY_DISABLED=1

    if [[ ! -f package.json || ! -f astro.config.mjs ]]; then
      echo "Run ./doc.sh from the async_nats repository. It shells into .#doc from docs/site." >&2
      exit 1
    fi

    mode="''${1:-serve}"
    npm ci
    npm run build

    if [[ "$mode" == "build" ]]; then
      echo "Documentation written to dist/"
      exit 0
    fi

    port="''${PORT:-4321}"
    echo "Documentation: http://127.0.0.1:''${port}/"
    exec npm run preview -- --host 127.0.0.1 --port "$port"
  '';
}
