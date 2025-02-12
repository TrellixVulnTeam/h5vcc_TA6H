// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/app_list/app_list_view.h"

#include <algorithm>

#include "base/string_util.h"
#include "ui/app_list/app_list_background.h"
#include "ui/app_list/app_list_constants.h"
#include "ui/app_list/app_list_item_model.h"
#include "ui/app_list/app_list_item_view.h"
#include "ui/app_list/app_list_model.h"
#include "ui/app_list/app_list_view_delegate.h"
#include "ui/app_list/contents_view.h"
#include "ui/app_list/pagination_model.h"
#include "ui/app_list/search_box_model.h"
#include "ui/app_list/search_box_view.h"
#include "ui/base/events/event.h"
#include "ui/gfx/insets.h"
#include "ui/gfx/path.h"
#include "ui/gfx/skia_util.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/widget/widget.h"

namespace app_list {

namespace {

// Inner padding space in pixels of bubble contents.
const int kInnerPadding = 1;

// The distance between the arrow tip and edge of the anchor view.
const int kArrowOffset = 10;

// The maximum allowed time to wait for icon loading in milliseconds.
const int kMaxIconLoadingWaitTimeInMs = 50;

}  // namespace

////////////////////////////////////////////////////////////////////////////////
// AppListView::IconLoader

class AppListView::IconLoader : public AppListItemModelObserver {
 public:
  IconLoader(AppListView* owner,
             AppListItemModel* item,
             ui::ScaleFactor scale_factor)
      : owner_(owner),
        item_(item) {
    item_->AddObserver(this);

    // Triggers icon loading for given |scale_factor|.
    item_->icon().GetRepresentation(scale_factor);
  }

  virtual ~IconLoader() {
    item_->RemoveObserver(this);
  }

 private:
  // AppListItemModelObserver overrides:
  virtual void ItemIconChanged() OVERRIDE {
    owner_->OnItemIconLoaded(this);
    // Note that IconLoader is released here.
  }
  virtual void ItemTitleChanged() OVERRIDE {}
  virtual void ItemHighlightedChanged() OVERRIDE {}

  AppListView* owner_;
  AppListItemModel* item_;

  DISALLOW_COPY_AND_ASSIGN(IconLoader);
};

////////////////////////////////////////////////////////////////////////////////
// AppListView:

AppListView::AppListView(AppListViewDelegate* delegate)
    : model_(new AppListModel),
      delegate_(delegate),
      search_box_view_(NULL),
      contents_view_(NULL) {
  if (delegate_)
    delegate_->SetModel(model_.get());
}

AppListView::~AppListView() {
  // Models are going away, ensure their references are cleared.
  RemoveAllChildViews(true);
  pending_icon_loaders_.clear();
}

void AppListView::InitAsBubble(
    gfx::NativeView parent,
    PaginationModel* pagination_model,
    views::View* anchor,
    const gfx::Point& anchor_point,
    views::BubbleBorder::ArrowLocation arrow_location) {
  // Starts icon loading early.
  PreloadIcons(pagination_model, anchor);

  SetLayoutManager(new views::BoxLayout(views::BoxLayout::kVertical,
                                        kInnerPadding,
                                        kInnerPadding,
                                        kInnerPadding));

  search_box_view_ = new SearchBoxView(this);
  AddChildView(search_box_view_);

  contents_view_ = new ContentsView(this, pagination_model);
  AddChildView(contents_view_);

  search_box_view_->set_contents_view(contents_view_);

  set_anchor_view(anchor);
  set_anchor_point(anchor_point);
  set_color(kContentsBackgroundColor);
  set_margins(gfx::Insets());
  set_move_with_anchor(true);
  set_parent_window(parent);
  set_close_on_deactivate(false);
  set_close_on_esc(false);
  set_anchor_insets(gfx::Insets(kArrowOffset, kArrowOffset,
                                kArrowOffset, kArrowOffset));
  set_shadow(views::BubbleBorder::BIG_SHADOW);
  views::BubbleDelegateView::CreateBubble(this);
  SetBubbleArrowLocation(arrow_location);

#if defined(USE_AURA)
  GetBubbleFrameView()->set_background(new AppListBackground(
      GetBubbleFrameView()->bubble_border()->GetBorderCornerRadius(),
      search_box_view_));

  contents_view_->SetPaintToLayer(true);
  contents_view_->SetFillsBoundsOpaquely(false);
  contents_view_->layer()->SetMasksToBounds(true);
  set_background(NULL);
#else
  set_background(new AppListBackground(
      GetBubbleFrameView()->bubble_border()->GetBorderCornerRadius(),
      search_box_view_));
#endif

  search_box_view_->SetModel(model_->search_box());
  contents_view_->SetModel(model_.get());
}

void AppListView::SetBubbleArrowLocation(
    views::BubbleBorder::ArrowLocation arrow_location) {
  GetBubbleFrameView()->bubble_border()->set_arrow_location(arrow_location);
  SizeToContents();  // Recalcuates with new border.
  GetBubbleFrameView()->SchedulePaint();
}

void AppListView::SetAnchorPoint(const gfx::Point& anchor_point) {
  set_anchor_point(anchor_point);
  SizeToContents();  // Repositions view relative to the anchor.
}

void AppListView::ShowWhenReady() {
  if (pending_icon_loaders_.empty()) {
    icon_loading_wait_timer_.Stop();
    GetWidget()->Show();
    return;
  }

  if (icon_loading_wait_timer_.IsRunning())
    return;

  icon_loading_wait_timer_.Start(
      FROM_HERE,
      base::TimeDelta::FromMilliseconds(kMaxIconLoadingWaitTimeInMs),
      this, &AppListView::OnIconLoadingWaitTimer);
}

void AppListView::Close() {
  icon_loading_wait_timer_.Stop();

  if (delegate_.get())
    delegate_->Dismiss();
  else
    GetWidget()->Close();
}

void AppListView::UpdateBounds() {
  SizeToContents();
}

void AppListView::PreloadIcons(PaginationModel* pagination_model,
                               views::View* anchor) {
  ui::ScaleFactor scale_factor = ui::SCALE_FACTOR_100P;
  if (anchor && anchor->GetWidget()) {
    scale_factor = ui::GetScaleFactorForNativeView(
        anchor->GetWidget()->GetNativeView());
  }

  // |pagination_model| could have -1 as the initial selected page and
  // assumes first page (i.e. index 0) will be used in this case.
  const int selected_page = std::max(0, pagination_model->selected_page());

  const int tiles_per_page = kPreferredCols * kPreferredRows;
  const int start_model_index = selected_page * tiles_per_page;
  const int end_model_index = std::min(
      static_cast<int>(model_->apps()->item_count()),
      start_model_index + tiles_per_page);

  pending_icon_loaders_.clear();
  for (int i = start_model_index; i < end_model_index; ++i) {
    AppListItemModel* item = model_->apps()->GetItemAt(i);
    if (item->icon().HasRepresentation(scale_factor))
      continue;

    pending_icon_loaders_.push_back(new IconLoader(this, item, scale_factor));
  }
}

void AppListView::OnIconLoadingWaitTimer() {
  GetWidget()->Show();
}

void AppListView::OnItemIconLoaded(IconLoader* loader) {
  ScopedVector<IconLoader>::iterator it = std::find(
      pending_icon_loaders_.begin(), pending_icon_loaders_.end(), loader);
  DCHECK(it != pending_icon_loaders_.end());
  pending_icon_loaders_.erase(it);

  if (pending_icon_loaders_.empty() && icon_loading_wait_timer_.IsRunning()) {
    icon_loading_wait_timer_.Stop();
    GetWidget()->Show();
  }
}

views::View* AppListView::GetInitiallyFocusedView() {
  return search_box_view_->search_box();
}

bool AppListView::WidgetHasHitTestMask() const {
  return true;
}

void AppListView::GetWidgetHitTestMask(gfx::Path* mask) const {
  DCHECK(mask);
  mask->addRect(gfx::RectToSkRect(
      GetBubbleFrameView()->GetContentsBounds()));
}

bool AppListView::AcceleratorPressed(const ui::Accelerator& accelerator) {
  // The accelerator is added by BubbleDelegateView.
  if (accelerator.key_code() == ui::VKEY_ESCAPE) {
    Close();
    return true;
  }

  return false;
}

void AppListView::ActivateApp(AppListItemModel* item, int event_flags) {
  if (delegate_.get())
    delegate_->ActivateAppListItem(item, event_flags);
}

void AppListView::QueryChanged(SearchBoxView* sender) {
  string16 query;
  TrimWhitespace(model_->search_box()->text(), TRIM_ALL, &query);
  bool should_show_search = !query.empty();
  contents_view_->ShowSearchResults(should_show_search);

  if (delegate_.get()) {
    if (should_show_search)
      delegate_->StartSearch();
    else
      delegate_->StopSearch();
  }
}

void AppListView::OpenResult(const SearchResult& result, int event_flags) {
  if (delegate_.get())
    delegate_->OpenSearchResult(result, event_flags);
}

void AppListView::InvokeResultAction(const SearchResult& result,
                                     int action_index,
                                     int event_flags) {
  if (delegate_.get())
    delegate_->InvokeSearchResultAction(result, action_index, event_flags);
}

void AppListView::OnWidgetClosing(views::Widget* widget) {
  BubbleDelegateView::OnWidgetClosing(widget);
  if (delegate_.get() && widget == GetWidget())
    delegate_->ViewClosing();
}

void AppListView::OnWidgetActivationChanged(views::Widget* widget,
                                            bool active) {
  // Do not called inherited function as the bubble delegate auto close
  // functionality is not used.
  if (delegate_.get() && widget == GetWidget())
    delegate_->ViewActivationChanged(active);
}

}  // namespace app_list
