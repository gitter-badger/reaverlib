/**
 * Reaver Library Licence
 *
 * Copyright (C) 2012-2013 Reaver Project Team:
 * 1. Michał "Griwes" Dominiak
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation is required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Michał "Griwes" Dominiak
 *
 **/

#include <type_traits>
#include <vector>
#include <tuple>

namespace reaver
{
    template<typename T>
    struct is_vector : public std::false_type
    {
    };

    template<typename T, typename A>
    struct is_vector<std::vector<T, A>> : public std::true_type
    {
    };

    template<typename T>
    struct is_tuple : public std::false_type
    {
    };

    template<typename... Ts>
    struct is_tuple<std::tuple<Ts...>> : public std::true_type
    {
    };

    template<typename T, typename U>
    struct make_tuple_type
    {
        using type = std::tuple<T, U>;
    };

    template<typename T, typename... Ts>
    struct make_tuple_type<T, std::tuple<Ts...>>
    {
        using type = std::tuple<T, Ts...>;
    };

    template<typename... Ts, typename T>
    struct make_tuple_type<std::tuple<Ts...>, T>
    {
        using type = std::tuple<Ts..., T>;
    };

    template<typename... T1s, typename... T2s>
    struct make_tuple_type<std::tuple<T1s...>, std::tuple<T2s...>>
    {
        using type = std::tuple<T1s..., T2s...>;
    };
}
