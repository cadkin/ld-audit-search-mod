final: prev: let
  glibcTargetVersion = "2.17";

  stdenvZig = (final.zig.override (old : {
    wrapCCWith = args: old.wrapCCWith (final.lib.recursiveUpdate args {
      nixSupport.cc-cflags = args.nixSupport.cc-cflags ++ [
        "-target" "${final.hostPlatform.system}-gnu.${glibcTargetVersion}"
      ];
    });
  })).stdenv;

  stdenvZigStatic = (final.makeStatic stdenvZig).override (old: {
    hostPlatform = old.hostPlatform // { isStatic = true; };
  });

  scope = final.lib.makeScope final.newScope (self: {
    stdenv = stdenvZigStatic;

    fmt = (final.fmt.override { inherit (self) stdenv; }).overrideAttrs (old: {
      cmakeFlags = (old.cmakeFlags or []) ++ [
        "-DFMT_TEST=OFF"
      ];
      doCheck = false;
    });

    spdlog = (final.spdlog.override { inherit (self) stdenv fmt; }).overrideAttrs (old: {
      cmakeFlags = (old.cmakeFlags or []) ++ [
        "-DSPDLOG_BUILD_TESTS=OFF"
      ];
      doCheck = false;
    });

    yaml-cpp = final.yaml-cpp.override { inherit (self) stdenv; };

    lasm = rec {
      audit = self.callPackage ./. { stdenv = stdenvZig; };

      integrations = {
        bash = self.callPackage ./integrations/bash { lasmConfig = configs.fallback; };
      };

      configs = {
        fallback = import ./configs/fallback.nix;
      };
    };

    ld-audit-search-mod = self.lasm.audit;
  });
in {
  inherit (scope) ld-audit-search-mod lasm;
}
