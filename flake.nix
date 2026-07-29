{
  description = "Logos Capability Module - Coordinates permissions between modules";

  inputs = {
    logos-module-builder = {
      # The scoped inputs deliberately stay on the known-compatible builder
      # revision until they are available from its default dependency graph.
      url = "github:logos-co/logos-module-builder/4717b9af35d88a20a960067ee55bc5417af5a1f0";
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
