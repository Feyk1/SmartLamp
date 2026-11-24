/* Opens and closes the help dialog for either panel. */
(() => {
  "use strict";

  const dialog = document.querySelector("[data-help-dialog]");
  if (!dialog) return;

  const open = () => {
    if (typeof dialog.showModal === "function") dialog.showModal();
    else dialog.setAttribute("open", "");
  };
  const close = () => {
    if (typeof dialog.close === "function") dialog.close();
    else dialog.removeAttribute("open");
  };

  document.querySelectorAll("[data-help-open]").forEach((button) => {
    button.addEventListener("click", open);
  });
  dialog.querySelectorAll("[data-help-close]").forEach((button) => {
    button.addEventListener("click", close);
  });
  dialog.addEventListener("click", (event) => {
    if (event.target === dialog) close();
  });
})();
