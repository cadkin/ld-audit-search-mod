{
  makeSetupHook, writeScript,

  ld-audit-search-mod,

  lasmConfig
}:

makeSetupHook {
  name = "lasmEnvHook";
} (
  writeScript "lasm-env-hook.sh" ''
    function enableLASM() {
      export LD_AUDIT=${ld-audit-search-mod}/lib/libld-audit-search-mod.so
      export LD_AUDIT_SEARCH_MOD_CONFIG=${lasmConfig.outPath}
    }

    addEnvHooks "$hostOffset" enableLASM
  ''
)

