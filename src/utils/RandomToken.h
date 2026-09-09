#ifndef NGPOST_RANDOMTOKEN_H
#define NGPOST_RANDOMTOKEN_H

#include <QRandomGenerator>
#include <QString>

namespace RandomToken {
inline QString fromGenerator(uint length, QRandomGenerator &generator)
{
    static const QString alphabet = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    // Rejection sampling: the accepted interval contains an exact multiple
    // of 62 values. Scaling a 32-bit integer directly would introduce bias.
    constexpr quint32 bound = 62;
    constexpr quint32 threshold = (quint32(0) - bound) % bound;
    QString token;
    for (uint i = 0; i < length; ++i) {
        quint32 value;
        do { value = generator.generate(); } while (value < threshold);
        token += alphabet.at(value % bound);
    }
    return token;
}
inline QString secret(uint length) { return fromGenerator(length, *QRandomGenerator::system()); }
// Public identifiers never consume a deterministic generator used for secrets.
inline QString publicName(uint length) { return fromGenerator(length, *QRandomGenerator::global()); }
}
#endif
