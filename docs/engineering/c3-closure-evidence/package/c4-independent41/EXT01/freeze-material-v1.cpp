#include "qcae/records.hpp"
#include <fstream>
int main() {
    const auto registry = qcae::make_record_registry();
    const auto record = registry->make(qcae::records::Material{
        qcae::EntityId("c4-legacy-material"), "Legacy Steel", 210000, .3});
    std::ofstream output("tests/fixtures/ext01-material-v1.record", std::ios::binary);
    output << record->encoded();
    return output.good() ? 0 : 1;
}
