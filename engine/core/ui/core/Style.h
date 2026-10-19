// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef UIStyle_h
#define UIStyle_h

namespace doriax::ui {

    // CSS-like length used by the layout properties: points (canvas pixels),
    // percent of the parent size, or auto. Some properties give "auto" a
    // specific meaning: margins center the element in flex containers, min/max
    // limits are lifted, width/height/flex-basis size from content.
    struct StyleLength {
        enum class Unit : unsigned char {
            Point,
            Percent,
            Auto
        };

        float value = 0.0f;
        Unit unit = Unit::Auto;

        StyleLength() = default;

        //implicit so the setters accept plain numbers, like CSS "12px"
        StyleLength(float pointValue) : value(pointValue), unit(Unit::Point) {}

        static StyleLength point(float value) {
            return StyleLength(value);
        }

        static StyleLength percent(float value) {
            StyleLength length;
            length.value = value;
            length.unit = Unit::Percent;
            return length;
        }

        static StyleLength autoSize() {
            return StyleLength();
        }
    };

    enum class Display : unsigned char {
        Flex,
        None
    };

    enum class FlexDirection : unsigned char {
        Row,
        RowReverse,
        Column,
        ColumnReverse
    };

    enum class FlexWrap : unsigned char {
        NoWrap,
        Wrap,
        WrapReverse
    };

    enum class JustifyContent : unsigned char {
        FlexStart,
        Center,
        FlexEnd,
        SpaceBetween,
        SpaceAround,
        SpaceEvenly
    };

    enum class AlignItems : unsigned char {
        FlexStart,
        Center,
        FlexEnd,
        Stretch,
        Baseline
    };

    enum class AlignSelf : unsigned char {
        Auto,
        FlexStart,
        Center,
        FlexEnd,
        Stretch,
        Baseline
    };

    enum class AlignContent : unsigned char {
        Auto,
        FlexStart,
        Center,
        FlexEnd,
        Stretch,
        SpaceBetween,
        SpaceAround
    };

    enum class PositionType : unsigned char {
        Relative,
        Absolute
    };

    //box edges; Horizontal covers left+right, Vertical covers top+bottom
    enum class Edge : unsigned char {
        Left,
        Top,
        Right,
        Bottom,
        Horizontal,
        Vertical,
        All
    };

    //Flexbox data of one element. Defaults follow the UMG/game-UI vocabulary:
    //row direction, stretch cross alignment and fixed sizes that do not shrink
    //(web pages shrink items by default; game panels keep their size). The
    //painters and the layout pass read this struct; a future USS resolver
    //writes into it.
    struct LayoutStyle {
        Display display = Display::Flex;
        PositionType positionType = PositionType::Relative;
        FlexDirection flexDirection = FlexDirection::Row;
        FlexWrap flexWrap = FlexWrap::NoWrap;
        JustifyContent justifyContent = JustifyContent::FlexStart;
        AlignItems alignItems = AlignItems::Stretch;
        AlignSelf alignSelf = AlignSelf::Auto;
        AlignContent alignContent = AlignContent::Auto;

        float flexGrow = 0.0f;
        float flexShrink = 0.0f;
        StyleLength flexBasis;

        StyleLength width;
        StyleLength height;
        StyleLength minWidth;
        StyleLength minHeight;
        StyleLength maxWidth;
        StyleLength maxHeight;

        StyleLength marginLeft = StyleLength::point(0.0f);
        StyleLength marginTop = StyleLength::point(0.0f);
        StyleLength marginRight = StyleLength::point(0.0f);
        StyleLength marginBottom = StyleLength::point(0.0f);

        StyleLength paddingLeft = StyleLength::point(0.0f);
        StyleLength paddingTop = StyleLength::point(0.0f);
        StyleLength paddingRight = StyleLength::point(0.0f);
        StyleLength paddingBottom = StyleLength::point(0.0f);

        StyleLength left;
        StyleLength top;
        StyleLength right;
        StyleLength bottom;

        float gap = 0.0f;
    };
}

#endif /* UIStyle_h */
