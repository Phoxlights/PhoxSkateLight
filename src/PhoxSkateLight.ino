#include <loop.h>
#include <animator.h>
#include <lightstrip.h>
#include <statuslight.h>
#include <alltransforms.h>
#include <network.h>
#include <ota.h>
#include <digitalbutton.h>
#include <objstore.h>
#include <eventReceiver.h>
#include <event.h>

#define ARRAY_SIZE(x) ((sizeof x) / (sizeof *x))

void asplode(char * err){
    Serial.printf("ERROR: %s\n", err);
    delay(1000);
    ESP.restart();
}

#define DB_VER 3

#define OTA_SSID "phoxlight"
#define OTA_PASS "phoxlight"
#define OTA_HOSTNAME "phoxlightota"

typedef struct SkateConfig {
    int numPx;
    int pin;
    char ssid[SSID_MAX];
    char pass[PASS_MAX];
    char hostname[HOSTNAME_MAX];
    int eventPort;
    int eventVer;
    int currentPreset;
    int buttonPin;
    int statusPin;
    NetworkMode networkMode;
} SkateConfig;

// default config values
SkateConfig defaultConfig = {
    4,
    2,
    "phoxlight",
    "phoxlight",
    "phoxlightskate",
    6767,
    1,
    0,
    14, //14 for skate light, 0 for dev
    2,
    CONNECT
};

int configId = 1;
SkateConfig config;
StatusLight status;

Animator animator;
LightStrip strip;
byte * buffer;

int writeDefaultConfig(){
    // delete existing skate config
    Serial.println("wiping skate config");
    objStoreWipe("skate");

    int id = objStoreCreate("skate", &defaultConfig, sizeof(SkateConfig));
    if(!id){
        Serial.println("failed to write default config");
        return 0;
    }
    Serial.printf("wrote default skate config, with id %i\n", id);
    return 1;
}

int writeCurrentConfig(){
    if(!objStoreUpdate("skate", configId, &config, sizeof(SkateConfig))){
        Serial.println("failed to write config");
        return 0;
    }
    return 1;
}

int loadConfig(){
    // load from fs
    objStoreInit(DB_VER);
    if(!objStoreGet("skate", configId, &config, sizeof(SkateConfig))){
        // store defaults
        Serial.println("skate config not found, storing defaults");
        if(!writeDefaultConfig()){
            asplode("couldnt store default skate config. prepare your butt.");
        }
        // reload config now that we have defaults
        loadConfig();
    }
    return 1;
}

void logConfig(){
    Serial.printf("\
config: {\n\
    numPx: %i,\n\
    pin: %i,\n\
    ssid: %s,\n\
    hostname: %s,\n\
    eventPort: %i,\n\
    eventVer: %i,\n\
    currentPreset: %i,\n\
    buttonPin: %i,\n\
    statusPin: %i,\n\
    networkMode: %s,\n\
}\n", 
    config.numPx, config.pin, config.ssid,
    config.hostname, config.eventPort, config.eventVer,
    config.currentPreset, config.buttonPin,
    config.statusPin,
    config.networkMode == 0 ? "CONNECT" : config.networkMode == 1 ? "CREATE" : "OFF");
}

void otaStarted(){
    Serial.println("ota start");
}

void otaProgress(unsigned int progress, unsigned int total){
    Serial.print("ota progress");
    animatorStop(animator);
    lightStripStop(strip);
    byte green[] = {0,255,0};
    int pattern[] = {500,50,0};
    statusLightSetPattern(status, green, pattern);
}

void otaError(ota_error_t err){
    Serial.println("ota err");
    byte red[] = {255,0,0};
    int pattern[] = {1000,50,0};
    statusLightSetPattern(status, red, pattern);
}

void otaEnd(){
    Serial.println("ota end");
}

void stopRunning2Layer(){
    AnimatorLayer l = animatorGetLayerAt(animator, 1);
    if(l != NULL){
        animatorLayerStop(l);
    }
}

// flashes status light real quick
void flash(){
    byte white[3] = {50,50,50};
    int pattern[] = {50, 100, 0};
    if(!statusLightSetPattern(status, white, pattern)){
        Serial.println("couldnt flash status light");
    }
    delay(50);
    statusLightStop(status);
}

void setNetworkMode(Event * e){
    int mode = (e->body[1] << 8) + e->body[0];
    Serial.printf("setting network mode to %i\n", mode); 
    config.networkMode = (NetworkMode)mode;
    writeCurrentConfig();
    delay(100);
    flash();
}

void restoreDefaultConfig(Event * e){
    Serial.println("restoring default config");
    if(!writeDefaultConfig()){
        Serial.println("could not restore default config");
        return;
    }
    Serial.println("restored default config");
    delay(100);
    flash();
}

void setButtonPin(Event * e){
    int pin = (e->body[1] << 8) + e->body[0];
    Serial.printf("setting button pin to %i\n", pin); 
    config.buttonPin = pin;
    writeCurrentConfig();
    delay(100);
    flash();
}

void nextPreset();

void nextPresetEvent(Event * e){
    Serial.println("next preset times");
    nextPreset();
}

void ping(Event * e){
    flash();
}

bool canOTA = true;
void neverOTAEver(){
    // button was released after boot, 
    // so don't allow OTA mode to happen
    canOTA = false;
}
void enterOTAMode(){
    Serial.println("entering OTA mode");

    // stop lights
    animatorStop(animator);
    lightStripStop(strip);

    // status light
    byte blue[3] = {0,0,40};
    byte red[3] = {40,0,0};
    int pattern[] = {500,50,0};
    if(!statusLightSetPattern(status, blue, pattern)){
        Serial.println("couldnt setup status light");
    }

    // stop network so it can be restarted in
    // connect mode
    if(!networkStop()){
        Serial.println("couldn't stop network");
    }

    Serial.printf("OTA attempting to connect to ssid: %s, pass: %s\n",
        OTA_SSID, OTA_PASS);

    if(!networkConnect(OTA_SSID, OTA_PASS)){
        Serial.println("couldnt connect to ota network");
        statusLightSetPattern(status, red, pattern);
        return;
    }
    networkAdvertise(OTA_HOSTNAME);
    Serial.printf("OTA advertising hostname: %s\n", OTA_HOSTNAME);

    // enable SET_NETWORK_MODE endpoint just in case it isnt,
    // this way a device with NETWORK_MODE off will be able to
    // be turned back on
    eventReceiverStart(config.eventVer, config.eventPort);
    eventReceiverRegister(SET_NETWORK_MODE, setNetworkMode);
    Serial.printf("Listening for SET_NETWORK_MODE with eventVer: %i, eventPort: %i\n",
        config.eventVer, config.eventPort);

    // ota
    otaOnStart(&otaStarted);
    otaOnProgress(&otaProgress);
    otaOnError(&otaError);
    otaOnEnd(&otaEnd);
    otaStart();

    byte green[3] = {0,40,0};
    int pattern2[] = {3000,50,0};
    if(!statusLightSetPattern(status, green, pattern2)){
        Serial.println("couldnt setup status light");
    }
}

byte RED[] = {255,0,0,255};
byte GREEN[] = {0,255,0,255};
byte BLUE[] = {0,0,255,255};
byte YELLOW[] = {255,255,0,255};
byte WHITE[] = {255,255,255,255};

static void addSpinnyLayer(){
    stopRunning2Layer();

    // there are only 4 px, but treating it as
    // 16 looks nicer
    int numPx = 16;
    AnimatorLayer layer = animatorLayerCreate(animator, numPx, 1, 0);

    byte bitmap_data[] = {
        255, 0, 0, 255,
        255, 0, 0, 127,
        255, 0, 0, 64,
        255, 0, 0, 32,
        255, 0, 0, 16,
        255, 0, 0, 32,
        255, 0, 0, 64,
        255, 0, 0, 127,
        255, 0, 0, 255,
        255, 0, 0, 127,
        255, 0, 0, 64,
        255, 0, 0, 32,
        255, 0, 0, 16,
        255, 0, 0, 32,
        255, 0, 0, 64,
        255, 0, 0, 127,
    };
    Bitmap * bmp1 = Bitmap_create(numPx, 1, bitmap_data);

    AnimatorKeyframe k1 = animatorKeyframeCreate(layer, 30, bmp1);
    animatorKeyframeAddTransform(k1, createTransformRGB(RED, GREEN, REPLACE));
    animatorKeyframeAddTransform(k1, createTransformTranslateX(0, 15, true));

    AnimatorKeyframe k2 = animatorKeyframeCreate(layer, 30, bmp1);
    animatorKeyframeAddTransform(k2, createTransformRGB(GREEN, BLUE, REPLACE));
    animatorKeyframeAddTransform(k2, createTransformTranslateX(0, 15, true));

    AnimatorKeyframe k3 = animatorKeyframeCreate(layer, 30, bmp1);
    animatorKeyframeAddTransform(k3, createTransformRGB(BLUE, RED, REPLACE));
    animatorKeyframeAddTransform(k3, createTransformTranslateX(0, 15, true));
}

static void addPulseLayer(byte * color){
    stopRunning2Layer();
    AnimatorLayer layer = animatorLayerCreate(animator, config.numPx, 1, 0);

    Bitmap * bmp = Bitmap_create(config.numPx, 1);
    Bitmap_fill(bmp, color);

    AnimatorKeyframe k1 = animatorKeyframeCreate(layer, 75, bmp);
    animatorKeyframeAddTransform(k1, createTransformPulse(15, 30, 15, 15));
}
static void addPulseRedLayer(){
    addPulseLayer(RED);
}
static void addPulseGreenLayer(){
    addPulseLayer(GREEN);
}
static void addPulseBlueLayer(){
    addPulseLayer(BLUE);
}
static void addPulseYellowLayer(){
    addPulseLayer(YELLOW);
}
static void addPulseWhiteLayer(){
    addPulseLayer(WHITE);
}

static void addSolidLayer(byte * color){
    stopRunning2Layer();
    AnimatorLayer layer = animatorLayerCreate(animator, config.numPx, 1, 0);

    Bitmap * bmp = Bitmap_create(config.numPx, 1);
    Bitmap_fill(bmp, color);

    AnimatorKeyframe k1 = animatorKeyframeCreate(layer, 75, bmp);
}
static void addSolidRedLayer(){
    addSolidLayer(RED);
}
static void addSolidGreenLayer(){
    addSolidLayer(GREEN);
}
static void addSolidBlueLayer(){
    addSolidLayer(BLUE);
}
static void addSolidYellowLayer(){
    addSolidLayer(YELLOW);
}
static void addSolidWhiteLayer(){
    addSolidLayer(WHITE);
}

static void addStrobeLayer(byte * color){
    stopRunning2Layer();
    AnimatorLayer layer = animatorLayerCreate(animator, config.numPx, 1, 0);

    Bitmap * bmp = Bitmap_create(config.numPx, 1);
    Bitmap_fill(bmp, color);

    AnimatorKeyframe k1 = animatorKeyframeCreate(layer, 5, bmp);
    animatorKeyframeAddTransform(k1, createTransformAlpha(0.0, 0.0));
    AnimatorKeyframe k2 = animatorKeyframeCreate(layer, 5, bmp);
    animatorKeyframeAddTransform(k2, createTransformAlpha(1.0, 1.0));
}
static void addStrobeRedLayer(){
    addStrobeLayer(RED);
}
static void addStrobeGreenLayer(){
    addStrobeLayer(GREEN);
}
static void addStrobeBlueLayer(){
    addStrobeLayer(BLUE);
}
static void addStrobeYellowLayer(){
    addStrobeLayer(YELLOW);
}
static void addStrobeWhiteLayer(){
    addStrobeLayer(WHITE);
}

static void addColoryLayer(){
    stopRunning2Layer();
    AnimatorLayer layer = animatorLayerCreate(animator, config.numPx, 1, 0);

    Bitmap * bmp = Bitmap_create(config.numPx, 1);
    Bitmap_fill(bmp, BLUE);

    AnimatorKeyframe k1 = animatorKeyframeCreate(layer, 30, bmp);
    animatorKeyframeAddTransform(k1, createTransformRGB(RED, GREEN, REPLACE));
    AnimatorKeyframe k2 = animatorKeyframeCreate(layer, 30, bmp);
    animatorKeyframeAddTransform(k2, createTransformRGB(GREEN, BLUE, REPLACE));
    AnimatorKeyframe k3 = animatorKeyframeCreate(layer, 30, bmp);
    animatorKeyframeAddTransform(k3, createTransformRGB(BLUE, RED, REPLACE));
}

static void addPulsyColoryLayer(){
    stopRunning2Layer();
    AnimatorLayer layer = animatorLayerCreate(animator, config.numPx, 1, 0);

    Bitmap * bmp = Bitmap_create(config.numPx, 1);
    Bitmap_fill(bmp, BLUE);

    AnimatorKeyframe k1 = animatorKeyframeCreate(layer, 31, bmp);
    animatorKeyframeAddTransform(k1, createTransformRGB(RED, GREEN, REPLACE));
    animatorKeyframeAddTransform(k1, createTransformPulse(10, 1, 10, 10));
    AnimatorKeyframe k2 = animatorKeyframeCreate(layer, 31, bmp);
    animatorKeyframeAddTransform(k2, createTransformRGB(GREEN, BLUE, REPLACE));
    animatorKeyframeAddTransform(k2, createTransformPulse(10, 1, 10, 10));
    AnimatorKeyframe k3 = animatorKeyframeCreate(layer, 31, bmp);
    animatorKeyframeAddTransform(k3, createTransformRGB(BLUE, RED, REPLACE));
    animatorKeyframeAddTransform(k3, createTransformPulse(10, 1, 10, 10));
}

static void addFlameLayer(){
    stopRunning2Layer();
    AnimatorLayer layer = animatorLayerCreate(animator, config.numPx, 1, 0);

    Bitmap * bmp = Bitmap_create(config.numPx, 1);
    byte orange[] = {255, 100, 0, 255};
    Bitmap_fill(bmp, orange);

    AnimatorKeyframe k1 = animatorKeyframeCreate(layer, 255, bmp);
    animatorKeyframeAddTransform(k1, createTransformFire(2, 4));
}

static void(*presets[])() = {
    addSpinnyLayer,
    addColoryLayer,
    addPulsyColoryLayer,
    addFlameLayer,
    addSolidRedLayer,
    addPulseRedLayer,
    addStrobeRedLayer,
    addSolidGreenLayer,
    addPulseGreenLayer,
    addStrobeGreenLayer,
    addSolidBlueLayer,
    addPulseBlueLayer,
    addStrobeBlueLayer,
    addSolidYellowLayer,
    addPulseYellowLayer,
    addStrobeYellowLayer,
    addSolidWhiteLayer,
    addPulseWhiteLayer,
    addStrobeWhiteLayer,
};
int presetCount = ARRAY_SIZE(presets);

int loadPreset(int preset){
    if(preset >= presetCount){
        Serial.printf("preset %i exceeds presetCount %i", preset, presetCount);
        return 0;
    }
    presets[preset]();
    return 1;
}

void nextPreset(){
    int nextPreset = ++config.currentPreset % presetCount;
    loadPreset(nextPreset);
    config.currentPreset = nextPreset;
    writeCurrentConfig();
}

int setupStartHeap, setupEndHeap, prevHeap;
void logHeapUsage(void * state){
    int currHeap = ESP.getFreeHeap();
    int delta = setupEndHeap - currHeap;
    Serial.printf("currHeap: %i, delta: %i\n", currHeap, delta);
    prevHeap = currHeap;
}

void setup(){
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n");

    setupStartHeap = ESP.getFreeHeap();
    Serial.printf("setupStartHeap: %i\n", setupStartHeap);

    // NOTE - config.statusPin is NOT used here
    // because this needs to be guaranteed to work
    status = statusLightCreate(2, 4);

    byte purple[3] = {20,0,20};
    int pattern[] = {1000,50,0};
    if(!statusLightSetPattern(status, purple, pattern)){
        Serial.println("couldnt setup status light");
    }

    // load config from fs
    loadConfig();
    logConfig();

    // status light
    byte blue[3] = {0,0,40};
    if(!statusLightSetPattern(status, blue, pattern)){
        Serial.println("couldnt setup status light");
    }

    // start network
    switch(config.networkMode){
        case CONNECT:
            if(!networkConnect(config.ssid, config.pass)){
                Serial.println("couldnt bring up network");
            }
            networkAdvertise(config.hostname);
            break;
        case CREATE:
            if(!networkCreate(config.ssid, config.pass, IPAddress(192,168,4,1))){
                Serial.println("couldnt create up network");
            }
            networkAdvertise(config.hostname);
            break;
        case OFF:
            Serial.println("turning network off");
            if(!networkOff()){
                Serial.println("couldnt turn off network");
            }
            break;
        default:
            Serial.println("couldnt load network mode, defaulting to CONNECT");
            if(!networkConnect(config.ssid, config.pass)){
                Serial.println("couldnt bring up network");
            }
            networkAdvertise(config.hostname);
            break;
    }

    byte orange[3] = {20,20,0};
    if(!statusLightSetPattern(status, orange, pattern)){
        Serial.println("couldnt setup status light");
    }

    if(eventReceiverStart(config.eventVer, config.eventPort)){
        eventReceiverRegister(SET_NETWORK_MODE, setNetworkMode);
        eventReceiverRegister(SET_DEFAULT_CONFIG, restoreDefaultConfig);
        eventReceiverRegister(SET_BUTTON_PIN, setButtonPin);
        eventReceiverRegister(NEXT_PRESET, nextPresetEvent);
    }

    // start up lights
    animator = animatorCreate(config.numPx, 1);
    if(animator == NULL){
        Serial.println("couldnt create animator");
    }
    buffer = animatorGetBuffer(animator)->data;
    strip = lightStripCreate(config.pin, config.numPx, 1.0, buffer);
    if(strip == NULL){
        Serial.println("couldnt create strip");
    }
    animatorPlay(animator);
    lightStripStart(strip);
    loadPreset(config.currentPreset);

    statusLightStop(status);

    // switch presets
    DigitalButton btn = buttonCreate(config.buttonPin, 200);
    buttonOnTap(btn, nextPreset);
    // OTA mode
    buttonOnUp(btn, neverOTAEver);
    buttonOnHold(btn, enterOTAMode, 4000);

    // debug log heap usage so i can keep an eye out for leaks
    setupEndHeap = ESP.getFreeHeap();
    Serial.printf("setupEndHeap: %i, delta: %i\n", setupEndHeap, setupStartHeap - setupEndHeap);
    loopAttach(logHeapUsage, 5000, NULL);
}

void loop(){
    loopTick();
}
