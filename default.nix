with import <nixpkgs> {
  crossSystem = {
    config = "arm-linux-gnueabihf";
    platform = (import <nixpkgs/lib>).systems.platforms.armv7l-hf-multiplatform;
  };
};

mkShell {
  buildInputs = [  ]; # your dependencies here
}
