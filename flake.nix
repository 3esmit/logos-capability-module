{
  description = "Logos Capability Module - Coordinates permissions between modules";

  inputs = {
    logos-module-builder = {
      # Keep module compilation on the maintained builder while its
      # host-service and universal-module contracts remain compatible.
      url = "github:3esmit/logos-module-builder?rev=3f4eb920469b69f7a8ee30ea34a88dbcdd624264";

      # Break the builder -> standalone-app -> capability-module cycle in this
      # core module's lock. UI modules must not copy this follows override.
      inputs.logos-standalone-app.follows = "";

      # The maintained liblogos input otherwise points back at this capability
      # module and recreates the same lock cycle one level deeper.
      inputs.logos-liblogos.inputs.logos-capability-module.follows = "";
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
