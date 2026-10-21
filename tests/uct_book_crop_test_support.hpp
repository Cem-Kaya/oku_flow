#pragma once

// Exact lossless RGB crop [630,252,46,36] of the owner's private source frame.
// Full PNG SHA256: df25907f060d7b8f8a59b39dfc1b633950500b0fc3941fe6a77eacee6089868f
// Extracted pixels only; no resampling, recoloring, font rendering or model.
#include <QByteArray>
#include <QImage>

namespace uct_book_crop_test {
inline constexpr auto kPackedRgbSha256 = "b8d7336e070cd539f50a35523e98efe03db377486fff5b122c83ec6016c9c36b";
inline QImage Image()
{
    return QImage::fromData(QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAC4AAAAkCAIAAAB5QJ8GAAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMA"
        "AA7DAcdvqGQAAAidSURBVFhHrZf9UxNJGsfzr12dd65vCLei64m6vuAeRl0XjuNYLVd3T9z1RM9VETkRUcDjRRQCQpDFAAsJMa9k"
        "3uiens709HRmEiLqVt3dj1ehYYwJstzWVn0r1clM8nzmOz3P842r2n100dTKSzZnGf5p0TRUcf/unZocO1V1+Ksva7o7WtOmNj3p"
        "ZVSlBChS5OD+cmE+eK3xYvejNjuFG/529vatRqDEng31xaN+i2n79paNPutnVO1+1Pb5yUo7hRUpcudmo66KzwZ6DlWU66qIlPi+"
        "3TtnJ8fE2EuCFMvQKAau+mq3TeDH238HhfCiqREo1JyotAlMEQilKEGSZai1Ne7JFyOUgKHBHs9At47l2R/HD1Tsam662tvdXld7"
        "cso3evbL6lhkFiPxwf3mjgctshhGMDE96Z3yjQrzwQzDNkVLaWNBCKdNLW1qC0KYIIliwHQ1RZChLbgavvrrm7TRdO3bS+frKZLa"
        "W25EA5NZhpNQ4N+3DDUY8IWCU6YBLaalTMSoyl8pAZQARtWkJiGYMA3IqGoakBKQzRimAfk6Y+u8PMVKikCbIo5Fcc4Py9BSBDFd"
        "dd3+x3dCeNZQRf5q6cDUZIYV/rUUgZahMloojERKgMU0B47z8TVncj60WI4jbWr8wihWbIpWf3wFxTI0F8OKpYM1xW8Tp9mwcj+d"
        "L5tirrSZtCkuPsGRyybQJpAXLhZH+VmavNMKC2xcOZRiPxw5NfKZ1n1bWMBRiiCu4kMrKIumlq8MRfkquGjHnhSBbBm06GhhgQ0q"
        "RZArbahpQy0gKEAploNSoOIaG9Q7lA+pGKLYpw2iOPuX61dD+RBQMcGaKsDiZL8QhbepXMtZvU2rh3Jliq+YizfWNVFsinMoxc+w"
        "o2IIzuH0q3xLmJ7r4hxlKW3mPywchSCF6WrWMqYnvD2d95/0dHa13/3Xw3uXL55PROZ+HsUpzIvZFL2ySdrUuCUUK9yY1dNW3Oav"
        "WcvgawzEFEG6KhvagqEtJKG0a+dWjvjKproqJ6G00uI+JG4DBongzATTgaHJy5eeE8WKockOCsXK8uxcockwnWLwyqbcDyke4igY"
        "iIa2AMRYyZZN3LAklIRocKXxryOCpBSBk+PDXe0tGYZTBCIlriRChiYbmpyEAhAjSSjw2Tbjy9nedP2Krsrz4cC335wL+adqTlUd"
        "qtgTD/mZrhKkICWRIigRmSsv3UYxEGMvxdjL3q52ghQXRRJFkqUDVYzE5qaQFFXFCIECRdKb5ZkenJnYt3tn7emqyfHhF2OepuuX"
        "I3NTzTeuXL9yEUpRpgMxNvfKJoOPu25cvSREg+5jh2Z8z4MzvvLSbQN9j5JQunC27kFrM9NVQ1tASiLD9HjIX166zdPf7R16Mvy0"
        "90lPp5KIuI4fqZBjcxRJNScqz9dXtzVfP/Pnkx2tTYumluOTY7oqnnYfjQanMUgcrzwgRAOqHENKfO+uHTM+L5Si/PFBSnxBCHv6"
        "uw9V7An5p8TYyx0f/XY+HNBV+fyZv3R3tPFQQpCSIige8u/auZXnJorBfDgApbhrenz44CdlGYpably52dhAoNDadK216ZoOEgwr"
        "iYg/RWDlp3vnw7OWof5hx++TUFASIYqVQxXlMz6vZagLQtgy1AnvYMutq0konao6Ojs5DsRY2fbNYuxl1jJqTlV5+rttivmepRj4"
        "p34o276ZYkCQwk3yDj1xxeamyks2L1n6xXN1fZ33oBC+dL6+5+HdXFPRgRANpAjcXbZVV0Wmgx0f/WY+PCvFgxQr+z8p43sFg4RN"
        "0YnPPg35fUko/bG8dMI7ZBlayZZNSEmo8vxp97HxkUGOwmObd+hJyZZNSiJiGVpgeuLz45W5FBeamThddVgVI7WnPhvz9EnRQH21"
        "+8XogKGKSjz4NkNNTT68b9f0+LAOEqMDPV+cqHxlk5HB3sD0OH+w3yyaGYabrl92Hzs4PjJ4tq7a09/dfvf23xsujHr6x0cG79y8"
        "NuEdUhIRpqsUA6QkMBAxEJNQ0lWZIIUjuno7Wv0+b5bhqsP7ZiZGVDFS7T7a1dZMoGCoIk90z4ceR/y+LMMZipJQyFo6d8KmKMMw"
        "T2WrD3mun/KglCIoaxm8tTh9tqD95791SdGATSBFUoYiJEUZVrIMZxl20p2pyRmK0obKQzjTQYbhrKU7CYFi5W2WGZq83A+xU95J"
        "JwVJpXj6rKDwWfPaJrxwhqJFU+N5ihNw/ee1bengbYY6CYF3OZui1xn6358WV7Nw4aXnoxRHp/zP341DxwabwIKhmN9/i6fx+yp0"
        "govpqqOCQ+9QeIFcSnqfw0lPxbO6qLwzC1f2ygZVYN57KHxdEOo2iJLvysa1Bgq/C3xvZhleM+dy8WzAVcTx7gb9X1rJKw6KI17y"
        "F6AUJ5s8FZYv4FgP5UNaB2VdrXFHCh7sXw2Fb9u8/ftuvbx4bzsXe7N2ti2ozT+xCeSzmhOsb8la/0s+CJEiiGKQtYz3XClYM6xA"
        "Icz3jaWDLMOWDniU5NMnRSBBEs8rTvkkFHiqsgx1KW0spQ3LUHVV5o04bSbzW36KoCSUkJLIjcORp935ltgELlk6f4gcY/ian8Bv"
        "jU0RQRKUokCMADGy2vWR7/kQ00ESCjzqEiQRJGUY5mOP6TkmjmUZmhQPUQwyTDe0hekJr+u7r89E/L6VfbDsREHD5Sj/XrJyluTt"
        "EqYDDBJAjCwIYZ6nMgz/6UhFyO9DSlyVY1CKhgOT4cAklKKqPM8HchJKBCkYiP6pHwb6Ht3/Z5MUD40MPt5dtt1l6QArcQfFUEU5"
        "NgeFsA4Spib3drR2tTVPjnkMVfT0dR7YUxoPzdRVu48c2APESNbSL3195sWY54sTlfPh2Z7OexV7Soefdr8Y89RVuy+crf2+seH7"
        "xoYLZ2vbWm4hJTE+MvhxyRYhGhwbfjrhHQJi7Jtz9Q/v3UFK4rT72P8A+ng/FeWWxiAAAAAASUVORK5CYII="
    ), "PNG").convertToFormat(QImage::Format_ARGB32);
}
} // namespace uct_book_crop_test
