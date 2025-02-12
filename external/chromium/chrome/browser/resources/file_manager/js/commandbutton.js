// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview This implements a common button control, bound to command.
 */

/**
 * Creates a new button element.
 * @param {Object=} opt_propertyBag Optional properties.
 * @constructor
 * @extends {HTMLDivElement}
 */
 var CommandButton = cr.ui.define('button');

/** @override */
CommandButton.prototype.__proto__ = HTMLButtonElement.prototype;

/**
 * Associated command.
 * @type {Command}
 * @private
 */
CommandButton.prototype.command_ = null;

/**
 * Initializes the menu item.
 */
CommandButton.prototype.decorate = function() {
  var commandId;
  if ((commandId = this.getAttribute('command')))
    this.setCommand(commandId);

  this.addEventListener('click', this.handleClick_.bind(this));
  this.addEventListener('keypress', this.handleKeyPress_.bind(this));
};

/**
 * Returns associated command.
 * @return {cr.ui.Command} associated command.
 */
CommandButton.prototype.getCommand = function() {
  return this.command_;
};

/**
 * Associates command with this button.
 * @param {String|cr.ui.Command} command Command id, or command object to
 * associate with this button.
 */
CommandButton.prototype.setCommand = function(command) {
  if (this.command_) {
    this.command_.removeEventListener('labelChange', this);
    this.command_.removeEventListener('disabledChange', this);
    this.command_.removeEventListener('hiddenChange', this);
  }

  if (typeof command == 'string' && command[0] == '#') {
    command = this.ownerDocument.getElementById(command.slice(1));
    cr.ui.decorate(command, cr.ui.Command);
  }

  this.command_ = command;
  if (command) {
    if (command.id)
      this.setAttribute('command', '#' + command.id);

    this.setLabel(command.label);
    this.disabled = command.disabled;
    this.hidden = command.hidden;

    this.command_.addEventListener('labelChange', this);
    this.command_.addEventListener('disabledChange', this);
    this.command_.addEventListener('hiddenChange', this);
  }
};

/**
 * Returns button label
 * @return {String} Button label.
 */
CommandButton.prototype.getLabel = function() {
  return this.textContent;
};

/**
 * Sets button label.
 * @param {string} label New button label.
 */
CommandButton.prototype.setLabel = function(label) {
  this.textContent = label;
};

/**
 * Handles click event and dispatches associated command.
 * @param {Event} e The mouseup event object.
 * @private
 */
CommandButton.prototype.handleClick_ = function(e) {
  if (!this.disabled && this.command_)
    this.command_.execute(this);
};

/**
 * Handles keypress event and dispatches associated command.
 * @param {Event} e The mouseup event object.
 * @private
 */
CommandButton.prototype.handleKeyPress_ = function(e) {
  if (!this.command_) return;

  switch (util.getKeyModifiers(e) + e.keyCode) {
    case '13':  // Enter
    case '32':  // Space
      this.command_.execute(this);
      break;
  }
};

/**
 * Handles changes to the associated command.
 * @param {Event} e The event object.
 */
CommandButton.prototype.handleEvent = function(e) {
  switch (e.type) {
    case 'disabledChange':
      this.disabled = this.command_.disabled;
      break;
    case 'hiddenChange':
      this.hidden = this.command_.hidden;
      break;
    case 'labelChange':
      this.setLabel(this.command_.label);
      break;
  }
};

/**
 * Whether the button is disabled or not.
 * @type {boolean}
 */
cr.defineProperty(CommandButton, 'disabled',
                  cr.PropertyKind.BOOL_ATTR);

/**
 * Whether the button is hidden or not.
 * @type {boolean}
 */
cr.defineProperty(CommandButton, 'hidden',
                  cr.PropertyKind.BOOL_ATTR);
