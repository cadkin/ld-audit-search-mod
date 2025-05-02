rec {
  config = {
    log_level = "error";
    rules = [
      {
        cond = {
          rtld = "nix";
          lib  = "(.*)";
        };

        default.prepend = [
          { dir = "/usr/lib/x86_64-linux-gnu"; }
        ];
      }
    ];
  };

  filename = "fallback.yaml";

  # TODO: use pipe (|>) when stable.
  outPath = builtins.toString (builtins.toFile filename (builtins.toJSON config));
}

