{
  description = "Logos Capability Module - Coordinates permissions between modules";

  inputs = {
    logos-module-builder = {
      # The scoped inputs deliberately stay on the known-compatible builder
      # revision until they are available from its default dependency graph.
      # Stacked on the maintained builder reconciliation; advance to its
      # merged revision before landing this module update.
      url = "github:3esmit/logos-module-builder?rev=e9d51fba2728a9cf6bbc3440b6b67d1c8917b263";
      # Override the builder's own SDK graph. This module has no module
      # dependencies, so exposing duplicate top-level inputs only expands the
      # lock file without affecting its build.
      inputs.logos-protocol.url = "github:3esmit/logos-protocol?rev=dbd1df94caeb3e073c330fc3d95988ce1086b1a5";
      inputs.logos-qt-sdk.url = "github:3esmit/logos-qt-sdk?rev=49cc49450de1db0168b687b52422beeefd55761c";
    };
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        tests = {
          dir = ./tests;
        };
      };
    in module // {
      checks = module.checks or {};
    };
}
