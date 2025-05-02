{
  writeTextFile,

  ld-audit-search-mod,

  lasmConfig
}:

writeTextFile rec {
  name = "lasm-bash-integration.sh";

  text = ''
    export LD_AUDIT=${ld-audit-search-mod}/lib/libld-audit-search-mod.so
    export LD_AUDIT_SEARCH_MOD_CONFIG=${lasmConfig.outPath}
  '';

  executable = true;

  destination = "/etc/profile.d/${name}";
}
