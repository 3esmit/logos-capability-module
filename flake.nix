{
  description = "Logos Capability Module - Coordinates permissions between modules";

  inputs = {
    logos-module-builder = {
      # Keep module compilation on the maintained builder while its
      # host-service metadata contract is carried through the dependency graph.
      url = "github:3esmit/logos-module-builder?rev=3f4eb920469b69f7a8ee30ea34a88dbcdd624264";
      # The builder revision carries the maintained Qt host and protocol pins;
      # keep its SDK graph intact so generator and host APIs stay compatible.
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
