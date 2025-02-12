// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/controls/table/table_utils.h"

#include "base/string_number_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/font.h"
#include "ui/views/controls/table/test_table_model.h"

using ui::TableColumn;
using ui::TableModel;

namespace views {

namespace {

std::string IntVectorToString(const std::vector<int>& values) {
  std::string result;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0)
      result += ",";
    result += base::IntToString(values[i]);
  }
  return result;
}

ui::TableColumn CreateTableColumnWithWidth(int width) {
  ui::TableColumn column;
  column.width = width;
  return column;
}

}

// Verifies columns with a specified width is honored.
TEST(TableUtilsTest, SetWidthHonored) {
  TestTableModel model(4);
  std::vector<TableColumn> columns;
  columns.push_back(CreateTableColumnWithWidth(20));
  columns.push_back(CreateTableColumnWithWidth(30));
  gfx::Font font;
  std::vector<int> result(
      CalculateTableColumnSizes(100, font, font, 0, columns, &model));
  EXPECT_EQ("20,30", IntVectorToString(result));

  // Same with some padding, it should be ignored.
  result = CalculateTableColumnSizes(100, font, font, 2, columns, &model);
  EXPECT_EQ("20,30", IntVectorToString(result));

  // Same with not enough space, it shouldn't matter.
  result = CalculateTableColumnSizes(10, font, font, 2, columns, &model);
  EXPECT_EQ("20,30", IntVectorToString(result));
}

// Verifies if no size is specified the last column gets all the available
// space.
TEST(TableUtilsTest, LastColumnGetsAllSpace) {
  TestTableModel model(4);
  std::vector<TableColumn> columns;
  columns.push_back(ui::TableColumn());
  columns.push_back(ui::TableColumn());
  gfx::Font font;
  std::vector<int> result(
      CalculateTableColumnSizes(500, font, font, 0, columns, &model));
  EXPECT_NE(0, result[0]);
  EXPECT_GE(result[1], WidthForContent(font, font, 0, columns[1], &model));
  EXPECT_EQ(500, result[0] + result[1]);
}

// Verifies a single column with a percent=1 is resized correctly.
TEST(TableUtilsTest, SingleResizableColumn) {
  TestTableModel model(4);
  std::vector<TableColumn> columns;
  columns.push_back(ui::TableColumn());
  columns.push_back(ui::TableColumn());
  columns.push_back(ui::TableColumn());
  columns[2].percent = 1.0f;
  gfx::Font font;
  std::vector<int> result(
      CalculateTableColumnSizes(500, font, font, 0, columns, &model));
  EXPECT_EQ(result[0], WidthForContent(font, font, 0, columns[0], &model));
  EXPECT_EQ(result[1], WidthForContent(font, font, 0, columns[1], &model));
  EXPECT_EQ(500 - result[0] - result[1], result[2]);

  // The same with a slightly larger width passed in.
  result = CalculateTableColumnSizes(1000, font, font, 0, columns, &model);
  EXPECT_EQ(result[0], WidthForContent(font, font, 0, columns[0], &model));
  EXPECT_EQ(result[1], WidthForContent(font, font, 0, columns[1], &model));
  EXPECT_EQ(1000 - result[0] - result[1], result[2]);

  // Just enough space to show the first two columns. Should force last column
  // to min size.
  result = CalculateTableColumnSizes(result[0] + result[1], font, font, 0,
                                     columns, &model);
  EXPECT_EQ(result[0], WidthForContent(font, font, 0, columns[0], &model));
  EXPECT_EQ(result[1], WidthForContent(font, font, 0, columns[1], &model));
  EXPECT_EQ(kUnspecifiedColumnWidth, result[2]);
}

}  // namespace views

