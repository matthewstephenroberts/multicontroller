import { useEffect } from "react";
import type { ReactNode } from "react";
import { X } from "lucide-react";

// In-app modal — replaces browser confirm()/prompt() dialogs, which can't be styled, block
// the whole tab, and have hostile cancel semantics (prompt() cancel returns null, and a
// careless Number(null) is 0 — which once taught a cancelled custom colour as id 0/black).
// Escape or clicking the backdrop cancels; actions are ordinary buttons supplied by the
// caller so each dialog controls its own validation before confirming.
export function Modal({ title, onClose, children, actions }: {
  title: string;
  onClose: () => void;
  children: ReactNode;
  actions: ReactNode;
}) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);

  return (
    <div className="modal-overlay" onMouseDown={(e) => { if (e.target === e.currentTarget) onClose(); }}>
      <div className="modal" role="dialog" aria-modal="true" aria-label={title}>
        <div className="modal-head">
          <h3>{title}</h3>
          <button className="ghost sm icon-btn" onClick={onClose} aria-label="Close"><X size={15} strokeWidth={2.25} /></button>
        </div>
        <div className="modal-body">{children}</div>
        <div className="modal-actions">{actions}</div>
      </div>
    </div>
  );
}

// One-question confirm dialog: message + Cancel / a single (usually destructive) action.
export function ConfirmModal({ title, message, confirmLabel, danger, onConfirm, onClose }: {
  title: string;
  message: ReactNode;
  confirmLabel: string;
  danger?: boolean;
  onConfirm: () => void;
  onClose: () => void;
}) {
  return (
    <Modal
      title={title}
      onClose={onClose}
      actions={
        <>
          <button className="ghost sm" onClick={onClose}>Cancel</button>
          <button
            className={`ghost sm${danger ? " danger" : ""}`}
            onClick={() => { onConfirm(); onClose(); }}
          >
            {confirmLabel}
          </button>
        </>
      }
    >
      <p style={{ margin: 0 }}>{message}</p>
    </Modal>
  );
}
