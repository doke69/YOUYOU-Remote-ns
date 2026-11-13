#pragma once
namespace uu {
void initializeThreadAffinity() noexcept;
void reportThreadAffinity(const char *phase) noexcept;
}
