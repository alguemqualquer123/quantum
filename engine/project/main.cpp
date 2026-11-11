#include "Quantum.h"
using namespace quantum;

#include "Image.h"

Scene scene;
Image image(&scene);

QUANTUM_INIT void init(){
    image.setAnchorPreset(AnchorPreset::CENTER);
    image.setTexture("quantum.png");

    Engine::setScalingMode(Scaling::FITWIDTH);
    Engine::setCanvasSize(1000,480);
    Engine::setScene(&scene);
}
