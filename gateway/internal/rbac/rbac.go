// Package rbac holds the permission checks: one function per permission, each table-tested.
// Callers load the facts (role, account mapping) from the database; these functions only decide.
package rbac

// Roles match the user_role enum.
const (
	RoleAdmin       = "admin"
	RoleRiskManager = "risk_manager"
	RoleTrader      = "trader"
	RoleAnalyst     = "analyst"
)

// Mapping is a user's row in account_user_map; nil means the user is not mapped to the account.
type Mapping struct {
	CanView  bool
	CanTrade bool
}

// CanBrokerLogin: start an account's daily broker login (API.md §7: mapped trader or admin). A
// trader needs can_trade, because the login starts the account's trading session.
func CanBrokerLogin(role string, m *Mapping) bool {
	switch role {
	case RoleAdmin:
		return true
	case RoleTrader:
		return m != nil && m.CanTrade
	}
	return false
}

// CanReceiveMarketData: live data from an account's feed goes only to users mapped to that account
// when it carries a data entitlement (ADR-009). This holds for every role, admin included.
func CanReceiveMarketData(m *Mapping, accountDataEntitled bool) bool {
	return m != nil && m.CanView && accountDataEntitled
}

// SeesAllAccounts: risk managers and admins see every account's status (API.md "scoped").
func SeesAllAccounts(role string) bool {
	return role == RoleAdmin || role == RoleRiskManager
}
