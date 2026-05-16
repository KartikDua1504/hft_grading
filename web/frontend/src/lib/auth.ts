import { writable } from 'svelte/store';
import { browser } from '$app/environment';

interface AuthState {
  token: string | null;
  team: string | null;
  authenticated: boolean;
}

const initial: AuthState = {
  token: browser ? localStorage.getItem('iicpc_token') : null,
  team: browser ? localStorage.getItem('iicpc_team') : null,
  authenticated: browser ? !!localStorage.getItem('iicpc_token') : false,
};

export const auth = writable<AuthState>(initial);

export function login(token: string, team: string) {
  if (browser) {
    localStorage.setItem('iicpc_token', token);
    localStorage.setItem('iicpc_team', team);
  }
  auth.set({ token, team, authenticated: true });
}

export function logout() {
  if (browser) {
    localStorage.removeItem('iicpc_token');
    localStorage.removeItem('iicpc_team');
  }
  auth.set({ token: null, team: null, authenticated: false });
}
