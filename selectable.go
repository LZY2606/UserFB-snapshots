package agouti

import (
	"github.com/sclevine/agouti/api"
	"github.com/sclevine/agouti/internal/element"
	"github.com/sclevine/agouti/internal/target"
)

type Selectors interface {
	String() string
}

type selectable struct {
	session   apiSession
	selectors target.Selectors
	frames    *frameContext
	framePath []string
}

type apiSession interface {
	element.Client
	Delete() error
	GetActiveElement() (*api.Element, error)
	GetWindow() (*api.Window, error)
	GetWindows() ([]*api.Window, error)
	SetWindow(window *api.Window) error
	SetWindowByName(name string) error
	DeleteWindow() error
	GetScreenshot() ([]byte, error)
	GetCookies() ([]*api.Cookie, error)
	SetCookie(cookie *api.Cookie) error
	DeleteCookie(name string) error
	DeleteCookies() error
	GetURL() (string, error)
	SetURL(url string) error
	GetTitle() (string, error)
	GetSource() (string, error)
	MoveTo(element *api.Element, point api.Offset) error
	Frame(frame *api.Element) error
	FrameParent() error
	Execute(body string, arguments []interface{}, result interface{}) error
	Forward() error
	Back() error
	Refresh() error
	GetAlertText() (string, error)
	SetAlertText(text string) error
	AcceptAlert() error
	DismissAlert() error
	NewLogs(logType string) ([]api.Log, error)
	GetLogTypes() ([]string, error)
	DoubleClick() error
	Click(button api.Button) error
	ButtonDown(button api.Button) error
	ButtonUp(button api.Button) error
	TouchDown(x, y int) error
	TouchUp(x, y int) error
	TouchMove(x, y int) error
	TouchClick(element *api.Element) error
	TouchDoubleClick(element *api.Element) error
	TouchLongClick(element *api.Element) error
	TouchFlick(element *api.Element, offset api.Offset, speed api.Speed) error
	TouchScroll(element *api.Element, offset api.Offset) error
	DeleteLocalStorage() error
	DeleteSessionStorage() error
	SetImplicitWait(timout int) error
	SetPageLoad(timout int) error
	SetScriptTimeout(timout int) error
}

// chained returns the frame context and frame path shared by selections
// derived from this selectable, together with the appended selectors.
func (s *selectable) chained(selectorType target.Type, value string) (target.Selectors, *frameContext, []string) {
	return s.selectors.Append(selectorType, value), s.frames, s.framePath
}

// Find finds exactly one element by CSS selector.
func (s *selectable) Find(selector string) *Selection {
	selectors, frames, path := s.chained(target.CSS, selector)
	return newSelection(s.session, frames, path, selectors.Single())
}

// FindByXPath finds exactly one element by XPath selector.
func (s *selectable) FindByXPath(selector string) *Selection {
	selectors, frames, path := s.chained(target.XPath, selector)
	return newSelection(s.session, frames, path, selectors.Single())
}

// FindByLink finds exactly one anchor element by its text content.
func (s *selectable) FindByLink(text string) *Selection {
	selectors, frames, path := s.chained(target.Link, text)
	return newSelection(s.session, frames, path, selectors.Single())
}

// FindByLabel finds exactly one element by associated label text.
func (s *selectable) FindByLabel(text string) *Selection {
	selectors, frames, path := s.chained(target.Label, text)
	return newSelection(s.session, frames, path, selectors.Single())
}

// FindByButton finds exactly one button element with the provided text.
// Supports <button>, <input type="button">, and <input type="submit">.
func (s *selectable) FindByButton(text string) *Selection {
	selectors, frames, path := s.chained(target.Button, text)
	return newSelection(s.session, frames, path, selectors.Single())
}

// FindByName finds exactly element with the provided name attribute.
func (s *selectable) FindByName(name string) *Selection {
	selectors, frames, path := s.chained(target.Name, name)
	return newSelection(s.session, frames, path, selectors.Single())
}

// FindByClass finds exactly one element with a given CSS class.
func (s *selectable) FindByClass(text string) *Selection {
	selectors, frames, path := s.chained(target.Class, text)
	return newSelection(s.session, frames, path, selectors.Single())
}

// FindByID finds exactly one element that has the given ID.
func (s *selectable) FindByID(id string) *Selection {
	selectors, frames, path := s.chained(target.ID, id)
	return newSelection(s.session, frames, path, selectors.Single())
}

// First finds the first element by CSS selector.
func (s *selectable) First(selector string) *Selection {
	selectors, frames, path := s.chained(target.CSS, selector)
	return newSelection(s.session, frames, path, selectors.At(0))
}

// FirstByXPath finds the first element by XPath selector.
func (s *selectable) FirstByXPath(selector string) *Selection {
	selectors, frames, path := s.chained(target.XPath, selector)
	return newSelection(s.session, frames, path, selectors.At(0))
}

// FirstByLink finds the first anchor element by its text content.
func (s *selectable) FirstByLink(text string) *Selection {
	selectors, frames, path := s.chained(target.Link, text)
	return newSelection(s.session, frames, path, selectors.At(0))
}

// FirstByLabel finds the first element by associated label text.
func (s *selectable) FirstByLabel(text string) *Selection {
	selectors, frames, path := s.chained(target.Label, text)
	return newSelection(s.session, frames, path, selectors.At(0))
}

// FirstByButton finds the first button element with the provided text.
// Supports <button>, <input type="button">, and <input type="submit">.
func (s *selectable) FirstByButton(text string) *Selection {
	selectors, frames, path := s.chained(target.Button, text)
	return newSelection(s.session, frames, path, selectors.At(0))
}

// FirstByName finds the first element with the provided name attribute.
func (s *selectable) FirstByName(name string) *Selection {
	selectors, frames, path := s.chained(target.Name, name)
	return newSelection(s.session, frames, path, selectors.At(0))
}

// FirstByClass finds the first element with a given CSS class.
func (s *selectable) FirstByClass(text string) *Selection {
	selectors, frames, path := s.chained(target.Class, text)
	return newSelection(s.session, frames, path, selectors.At(0))
}

// All finds zero or more elements by CSS selector.
func (s *selectable) All(selector string) *MultiSelection {
	selectors, frames, path := s.chained(target.CSS, selector)
	return newMultiSelection(s.session, frames, path, selectors)
}

// AllByXPath finds zero or more elements by XPath selector.
func (s *selectable) AllByXPath(selector string) *MultiSelection {
	selectors, frames, path := s.chained(target.XPath, selector)
	return newMultiSelection(s.session, frames, path, selectors)
}

// AllByLink finds zero or more anchor elements by its text content.
func (s *selectable) AllByLink(text string) *MultiSelection {
	selectors, frames, path := s.chained(target.Link, text)
	return newMultiSelection(s.session, frames, path, selectors)
}

// AllByLabel finds zero or more elements by associated label text.
func (s *selectable) AllByLabel(text string) *MultiSelection {
	selectors, frames, path := s.chained(target.Label, text)
	return newMultiSelection(s.session, frames, path, selectors)
}

// AllByButton finds zero or more button elements with the provided text.
// Supports <button>, <input type="button">, and <input type="submit">.
func (s *selectable) AllByButton(text string) *MultiSelection {
	selectors, frames, path := s.chained(target.Button, text)
	return newMultiSelection(s.session, frames, path, selectors)
}

// AllByName finds zero or more elements with the provided name attribute.
func (s *selectable) AllByName(name string) *MultiSelection {
	selectors, frames, path := s.chained(target.Name, name)
	return newMultiSelection(s.session, frames, path, selectors)
}

// AllByClass finds zero or more elements with a given CSS class.
func (s *selectable) AllByClass(text string) *MultiSelection {
	selectors, frames, path := s.chained(target.Class, text)
	return newMultiSelection(s.session, frames, path, selectors)
}

// AllByID finds zero or more elements with a given ID.
func (s *selectable) AllByID(text string) *MultiSelection {
	selectors, frames, path := s.chained(target.ID, text)
	return newMultiSelection(s.session, frames, path, selectors)
}

// FirstByClass finds the first element with a given CSS class.
func (s *selectable) FindForAppium(selectorType string, text string) *Selection {
	selectors, frames, path := s.chained(target.Class, text)
	return newSelection(s.session, frames, path, selectors.At(0))
}

func (s *selectable) Selectors() Selectors {
	return s.selectors
}
