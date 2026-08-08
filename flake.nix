{
  description = "Logos Capability Module - Coordinates permissions between modules";

  inputs = {
    logos-module-builder = {
      # The scoped inputs deliberately stay on the known-compatible builder
      # revision until they are available from its default dependency graph.
      # Stacked on the maintained builder reconciliation; advance to its
      # merged revision before landing this module update.
      url = "github:3esmit/logos-module-builder?rev=e1ea6f1bb90d97666049025cd8c1604e8e284c6b";
      # Override the builder's own SDK graph. This module has no module
      # dependencies, so exposing duplicate top-level inputs only expands the
      # lock file without affecting its build.
      inputs.logos-protocol.url = "github:3esmit/logos-protocol";
      inputs.logos-qt-sdk.url = "github:3esmit/logos-qt-sdk";
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
