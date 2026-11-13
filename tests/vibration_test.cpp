#include "vibration.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using uu::vibration::Rumble;

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

static std::optional<Rumble> parseText(const std::string &text) {
    return uu::vibration::parse(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

static bool near(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

int main() {
    try {
        auto rumble = parseText(R"({"action":"gpd_vibration","index":0,"left_motor":200,"right_motor":40})");
        require(rumble && rumble->left == 200 && rumble->right == 40, "valid message");
        rumble = parseText(R"({"action":"gpd_vibration","index":0,"left_motor":999,"right_motor":-5})");
        require(rumble && rumble->left == 255 && rumble->right == 0, "motors clamp to 0..255");
        rumble = parseText(R"({"action":"gpd_vibration","left_motor":12.7,"right_motor":0})");
        require(rumble && rumble->left == 12 && rumble->right == 0, "missing index means pad 0");
        rumble = parseText(R"({"action":"gpd_vibration","index":0,"right_motor":90})");
        require(rumble && rumble->left == 0 && rumble->right == 90, "one motor field is enough");

        rumble=parseText(R"({"action":"gpd_vibration","index":1,"left_motor":1,"right_motor":1})");
        require(rumble && rumble->index==1,"second pad index");
        require(!parseText(R"({"action":"gpd_vibration","index":2,"left_motor":1})"),"unsupported pad index");
        require(!parseText(R"({"action":"gpd_vibration","index":"0","left_motor":1})"), "string index");
        require(!parseText(R"({"action":"gpd_update","index":0,"left_motor":1})"), "other action");
        require(!parseText(R"({"action":"gpd_vibration","index":0})"), "no motor fields");
        require(!parseText(R"({"action":"gpd_vibration","index":0,"left_motor":"9"})"), "string motor");
        require(!parseText(R"( {"action":"gpd_vibration","left_motor":1})"), "must start with brace");
        require(!parseText(R"({"action":"gpd_vibration","left_motor":1)"), "truncated json");
        require(!parseText(R"(["gpd_vibration"])"), "array");
        require(!parseText(R"({"action":"other","note":"gpd_vibration"})"), "substring alone is not enough");
        require(!uu::vibration::parse(nullptr, 0), "null input");
        std::string withNul = R"({"action":"gpd_vibration","left_motor":1})";
        withNul.insert(5, 1, '\0');
        require(!parseText(withNul), "embedded NUL");
        std::string large = R"({"action":"gpd_vibration","left_motor":1,"pad":")" + std::string(600, 'x') + "\"}";
        require(!parseText(large), "oversized message");

        require(near(uu::vibration::amplitude(255, 100), .8f), "full scale is 0.8");
        require(near(uu::vibration::amplitude(255, 60), .48f), "strength scales");
        require(near(uu::vibration::amplitude(0, 100), 0), "zero level");
        require(near(uu::vibration::amplitude(255, 0), 0), "strength off");
        require(near(uu::vibration::amplitude(1000, 1000), .8f), "inputs clamp");
        require(near(uu::vibration::amplitude(-4, 100), 0), "negative level clamps");

        const Rumble on{120, 60};
        require(uu::vibration::effective(on, 1000, 1200, true, 300) == on, "fresh value passes");
        require(uu::vibration::effective(on, 1000, 1300, true, 300) == on, "hold boundary passes");
        require(uu::vibration::effective(on, 1000, 1301, true, 300) == Rumble{}, "watchdog zeroes stale value");
        require(uu::vibration::effective(on, 1000, 1100, false, 300) == Rumble{}, "gate closed");
        require(uu::vibration::effective(on, 0, 100, true, 300) == Rumble{}, "never received");

        uu::vibration::setStrength(150);
        require(uu::vibration::strength() == 100, "strength clamps high");
        uu::vibration::setStrength(-3);
        require(uu::vibration::strength() == 0, "strength clamps low");
        uu::vibration::setStrength(60);
        require(uu::vibration::strength() == 60, "strength stored");

        uu::vibration::submit({10, 20});
        uu::vibration::tick(0, 0, true);
        uu::vibration::stop();
        uu::vibration::endSession();
        uu::vibration::shutdown();
        uu::vibration::submit({10, 20});

        std::cout << "PASS: vibration parse/clamp/reject, amplitude scale, watchdog/gate, strength\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
